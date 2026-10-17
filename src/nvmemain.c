//
// SCSI Miniport Driver for Windows 2000
//

#include "nvment.h"
#include "nvmeutil.h"

// The crash dump / hibernation stack calls DriverEntry without a driver object
static BOOLEAN g_DumpMode;

#ifdef NVMENT_W9X
//
// Windows 9x: VMM _MapPhysToLinear (int 20h VxDCall, VMM device 0001h,
// service 006Ch); SCSIPORT.PDR cannot map memory above 16 MB itself
//
#define NVMENT_VMM_DEVICE_ID            0x0001
#define NVMENT_MAP_PHYS_TO_LINEAR       0x006C

#ifdef _MSC_VER
__declspec(naked) PVOID __cdecl
NvmeMapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG Flags)
{
    __asm {
        int 0x20
        _emit ((NVMENT_MAP_PHYS_TO_LINEAR >> 0) & 0xFF)
        _emit (((NVMENT_MAP_PHYS_TO_LINEAR >> 8) & 0xFF) | 0x80)
        _emit ((NVMENT_VMM_DEVICE_ID >> 0) & 0xFF)
        _emit ((NVMENT_VMM_DEVICE_ID >> 8) & 0xFF)
    }
}
#else
__attribute__((naked)) PVOID __cdecl
NvmeMapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG Flags)
{
    __asm__ volatile(".byte 0xCD, 0x20, 0x6C, 0x80, 0x01, 0x00");
}
#endif

// Several NVMe controllers: every one is claimed by the instance that
// found it, so the next instance does not take it again
static struct {
    ULONG Bus;
    ULONG Slot;
} g_Claimed[NVMENT_MAX_CONTROLLERS];
static ULONG g_ClaimedCount;

static BOOLEAN NvmeIsClaimed(IN ULONG Bus, IN ULONG Slot)
{
    ULONG i;
    for (i = 0; i < g_ClaimedCount; i++) {
        if (g_Claimed[i].Bus == Bus && g_Claimed[i].Slot == Slot) return TRUE;
    }
    return FALSE;
}

static VOID NvmeClaim(IN ULONG Bus, IN ULONG Slot)
{
    if (g_ClaimedCount < NVMENT_MAX_CONTROLLERS && !NvmeIsClaimed(Bus, Slot)) {
        g_Claimed[g_ClaimedCount].Bus = Bus;
        g_Claimed[g_ClaimedCount].Slot = Slot;
        g_ClaimedCount++;
    }
}
#endif

#ifdef NVMENT_NT31_COMPAT
// NT 3.1: ScsiPortInitialize refuses the NT 3.50+ structure size before
// HwFindAdapter is ever called
static BOOLEAN g_FindAdapterCalled;
#endif

// --------------------------------------------------------------------------
// IRQ robustness for Win2000/XP (SCSIPORT): force legacy INTx by disabling MSI/MSI-X
// Many modern NVMe controllers come up with MSI-X enabled; XP SCSIPORT does not
// reliably support MSI/MSI-X, resulting in massive timeouts (SpTimeoutSynchronized...).
// --------------------------------------------------------------------------
static VOID NvmeForceLegacyIntx(IN PHW_DEVICE_EXTENSION DevExt)
{
    // PCI conventional header offsets
    const UCHAR PCI_STATUS_OFFSET_LOCAL   = 0x06; // USHORT
    const UCHAR PCI_CAP_PTR_OFFSET_LOCAL  = 0x34; // UCHAR
    const USHORT PCI_STATUS_CAP_LIST      = 0x0010;

    const UCHAR PCI_CAP_ID_MSI  = 0x05;
    const UCHAR PCI_CAP_ID_MSIX = 0x11;

    USHORT status;
    UCHAR  capPtr;
    UCHAR  capId;
    UCHAR  next;
    ULONG  guard;

    status = ReadPciConfigWord(DevExt, PCI_STATUS_OFFSET_LOCAL);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        return;
    }

    capPtr = ReadPciConfigByte(DevExt, PCI_CAP_PTR_OFFSET_LOCAL);
    capPtr = (UCHAR)(capPtr & (UCHAR)~0x03); // DWORD alignment

    guard = 0;
    while (guard < 32 && capPtr >= 0x40 && capPtr < 0x100) {
        capId  = ReadPciConfigByte(DevExt, capPtr);
        next   = ReadPciConfigByte(DevExt, (UCHAR)(capPtr + 1));
        next   = (UCHAR)(next & (UCHAR)~0x03);

        if (capId == PCI_CAP_ID_MSI) {
            // MSI Message Control is a WORD at cap+2; bit0 = MSI Enable
            USHORT msiCtrl;
            USHORT newCtrl;

            msiCtrl = ReadPciConfigWord(DevExt, (UCHAR)(capPtr + 2));
            if (msiCtrl & 0x0001) {
                newCtrl = (USHORT)(msiCtrl & (USHORT)~0x0001);
                WritePciConfigWord(DevExt, (UCHAR)(capPtr + 2), newCtrl);
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NvmeForceLegacyIntx - MSI disabled (cap=%02X ctrl %04X->%04X)\n",
                               capPtr, msiCtrl, newCtrl);
#endif
            }
        } else if (capId == PCI_CAP_ID_MSIX) {
            // MSI-X Message Control is a WORD at cap+2; bit15 = MSI-X Enable, bit14 = Function Mask
            USHORT msixCtrl;
            USHORT newCtrl;

            msixCtrl = ReadPciConfigWord(DevExt, (UCHAR)(capPtr + 2));
            newCtrl  = msixCtrl;

            // Mask vectors and disable MSI-X
            newCtrl = (USHORT)(newCtrl | 0x4000);           // Function Mask
            newCtrl = (USHORT)(newCtrl & (USHORT)~0x8000);  // MSI-X Enable

            if (newCtrl != msixCtrl) {
                WritePciConfigWord(DevExt, (UCHAR)(capPtr + 2), newCtrl);
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NvmeForceLegacyIntx - MSI-X disabled (cap=%02X ctrl %04X->%04X)\n",
                               capPtr, msixCtrl, newCtrl);
#endif
            }
        }

        if (next == 0 || next == capPtr) {
            break;
        }

        capPtr = next;
        guard++;
    }
}

#if (_WIN32_WINNT >= 0x500)
//
// Text-mode setup (F6) boots with a bare service key: the [Config] values of
// txtsetup.oem reach only the installed system. Without
// Parameters\PnpInterface the XP x64 / Server 2003 x64 ScsiPort fails
// IRP_MN_START_DEVICE before HwFindAdapter (STOP 0x7B), so the driver adds
// the entry the INF would have made. Declared here: miniport.h has no
// registry API.
//
typedef struct _NVMENT_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    USHORT *Buffer;
} NVMENT_UNICODE_STRING, *PNVMENT_UNICODE_STRING;

typedef struct _NVMENT_OBJECT_ATTRIBUTES {
    ULONG Length;
    PVOID RootDirectory;
    PNVMENT_UNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} NVMENT_OBJECT_ATTRIBUTES;

LONG __stdcall ZwOpenKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes);
LONG __stdcall ZwCreateKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
                           ULONG TitleIndex, PVOID Class, ULONG CreateOptions, PULONG Disposition);
LONG __stdcall ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize);
LONG __stdcall ZwClose(PVOID Handle);

#define NVMENT_OBJ_CASE_INSENSITIVE     0x00000040
#define NVMENT_OBJ_KERNEL_HANDLE        0x00000200
#define NVMENT_KEY_READ_WRITE           0x0002001F  // KEY_READ | KEY_WRITE
#define NVMENT_REG_CREATED_NEW_KEY      1
#define NVMENT_REG_DWORD                4

static USHORT g_ParametersName[] = L"Parameters";
static USHORT g_PnpInterfaceName[] = L"PnpInterface";
static USHORT g_PciBusValueName[] = L"5";              // PCIBus

static VOID NvmeInitString(OUT PNVMENT_UNICODE_STRING String, IN USHORT *Text, IN ULONG Bytes)
{
    String->Buffer = Text;
    String->Length = (USHORT)(Bytes - sizeof(USHORT));
    String->MaximumLength = (USHORT)Bytes;
}

static VOID NvmeInitAttributes(OUT NVMENT_OBJECT_ATTRIBUTES *Oa, IN PNVMENT_UNICODE_STRING Name, IN PVOID Root)
{
    Oa->Length = sizeof(NVMENT_OBJECT_ATTRIBUTES);
    Oa->RootDirectory = Root;
    Oa->ObjectName = Name;
    Oa->Attributes = NVMENT_OBJ_CASE_INSENSITIVE | NVMENT_OBJ_KERNEL_HANDLE;
    Oa->SecurityDescriptor = NULL;
    Oa->SecurityQualityOfService = NULL;
}

static VOID NvmeEnsurePnpInterface(IN PVOID RegistryPath)
{
    NVMENT_OBJECT_ATTRIBUTES oa;
    NVMENT_UNICODE_STRING name;
    PVOID service, params, pnp;
    ULONG disposition = 0;
    ULONG one = 1;
    LONG status;

    if (RegistryPath == NULL) {
        return;
    }

    NvmeInitAttributes(&oa, (PNVMENT_UNICODE_STRING)RegistryPath, NULL);
    if (ZwOpenKey(&service, NVMENT_KEY_READ_WRITE, &oa) < 0) {
        return;
    }

    NvmeInitString(&name, g_ParametersName, sizeof(g_ParametersName));
    NvmeInitAttributes(&oa, &name, service);
    status = ZwCreateKey(&params, NVMENT_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
    if (status >= 0) {
        NvmeInitString(&name, g_PnpInterfaceName, sizeof(g_PnpInterfaceName));
        NvmeInitAttributes(&oa, &name, params);
        status = ZwCreateKey(&pnp, NVMENT_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
        if (status >= 0) {
            if (disposition == NVMENT_REG_CREATED_NEW_KEY) {
                NvmeInitString(&name, g_PciBusValueName, sizeof(g_PciBusValueName));
                status = ZwSetValueKey(pnp, &name, 0, NVMENT_REG_DWORD, &one, sizeof(one));
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: DriverEntry - Parameters\\PnpInterface\\5 added (%08X)\n", (ULONG)status);
#endif
            }
            ZwClose(pnp);
        }
        ZwClose(params);
    }
    ZwClose(service);
}
#endif

//
// DriverEntry - Main entry point
//
ULONG DriverEntry(IN PVOID DriverObject, IN PVOID Argument2)
{
    HW_INITIALIZATION_DATA hwInitData;
    ULONG status;
#if (_WIN32_WINNT < 0x500)
    ULONG HwContext[2];
#endif

    g_DumpMode = (BOOLEAN)(DriverObject == NULL);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: DriverEntry called%s\n", g_DumpMode ? " (dump)" : "");
#endif

#if (_WIN32_WINNT >= 0x500)
    if (!g_DumpMode) {
        NvmeEnsurePnpInterface(Argument2);
    }
#endif

    // Zero out the initialization data structure
    memset(&hwInitData, 0, sizeof(HW_INITIALIZATION_DATA));

    // Set size of structure: NT 3.50-4.0 accept it up to DeviceId
#ifdef NVMENT_NT31_COMPAT
    hwInitData.HwInitializationDataSize = NVMENT_HWINIT_SIZE_NT35;
#else
    hwInitData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
#endif

    // Set entry points
    hwInitData.HwInitialize = HwInitialize;
    hwInitData.HwStartIo = HwStartIo;
    hwInitData.HwInterrupt = HwInterrupt;
    hwInitData.HwFindAdapter = HwFindAdapter;
    hwInitData.HwResetBus = HwResetBus;
#if (_WIN32_WINNT >= 0x500)
    hwInitData.HwAdapterControl = HwAdapterControl;
#else
    hwInitData.HwAdapterState = HwAdapterState;
#endif

    // Set driver-specific parameters
    hwInitData.AdapterInterfaceType = PCIBus;  // Change as needed
    hwInitData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    hwInitData.SpecificLuExtensionSize = 0;
    hwInitData.SrbExtensionSize = sizeof(NVME_SRB_EXTENSION);  // Required for PRP list tracking
    hwInitData.NumberOfAccessRanges = 1;
    hwInitData.MapBuffers = TRUE;
    hwInitData.NeedPhysicalAddresses = TRUE;
    // The dump stack completes one request at a time (Fast Polling)
    hwInitData.TaggedQueuing = (BOOLEAN)!g_DumpMode;
    hwInitData.AutoRequestSense = TRUE;
    hwInitData.MultipleRequestPerLu = (BOOLEAN)!g_DumpMode;
#ifdef NVMENT_W9X
    hwInitData.TaggedQueuing = FALSE;
    hwInitData.MultipleRequestPerLu = FALSE;
#endif

#if (_WIN32_WINNT < 0x500)
    // Vendor/Device IDs (for PCI devices)
    hwInitData.VendorIdLength = 0;
    hwInitData.VendorId = NULL;
    hwInitData.DeviceIdLength = 0;
    hwInitData.DeviceId = NULL;

    HwContext[0] = 0;
    HwContext[1] = 0;
#endif
    // Call port driver
    status = ScsiPortInitialize(DriverObject, Argument2,
                                &hwInitData, 
#if (_WIN32_WINNT >= 0x500)
                                NULL
#else
                                HwContext
#endif
                            );

#ifdef NVMENT_NT31_COMPAT
    // NT 3.1 refuses any other structure size before HwFindAdapter is ever
    // called, and has no PCI bus to scan: retry with its 40h-byte layout on
    // the ISA bus. The PCI functions are then found through the ports.
    if (status != 0 && !g_FindAdapterCalled) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: DriverEntry - status 0x%08X, retrying with the NT 3.1 layout\n", status);
#endif
        hwInitData.HwInitializationDataSize = NVMENT_HWINIT_SIZE_NT31;
        hwInitData.AdapterInterfaceType = Isa;
        HwContext[0] = 0;
        HwContext[1] = 0;
        status = ScsiPortInitialize(DriverObject, Argument2, &hwInitData, HwContext);
    }
#endif

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: DriverEntry exiting with status 0x%08X\n", status);
#endif
    return status;
}

ULONG HwFoundAdapter(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    IN PUCHAR pciBuffer,
    IN PCHAR ArgumentString,
    IN BOOLEAN HasSlot)
{
    PACCESS_RANGE accessRange;
    SCSI_PHYSICAL_ADDRESS bar0;
    ULONG bar0tmp;
    ULONG barSize;
    ULONG tempSize;
    ULONG mapBus;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - found NVMe device VID=%04X DID=%04X at bus %d slot %d\n",
                   DevExt->VendorId, DevExt->DeviceId, DevExt->BusNumber, DevExt->SlotNumber);
#endif

    // Read subsystem IDs using helper function
    DevExt->SubsystemVendorId = ReadPciConfigWord(DevExt, PCI_SUBSYSTEM_VENDOR_ID_OFFSET);
    DevExt->SubsystemId = ReadPciConfigWord(DevExt, PCI_SUBSYSTEM_ID_OFFSET);

    // Enable PCI device decoding and bus mastering.
    // Do NOT leave PCI_INTERRUPT_DISABLE set here: on Win2000/XP ScsiPort setups frequently rely on legacy INTx.
    // (MSI/MSI-X is not guaranteed/available with ScsiPort.)
    {
        USHORT cmdReg = ReadPciConfigWord(DevExt, PCI_COMMAND_OFFSET);
        cmdReg |= (PCI_ENABLE_BUS_MASTER | PCI_ENABLE_MEMORY_SPACE);
        cmdReg = (USHORT)(cmdReg & ~PCI_INTERRUPT_DISABLE); // ensure INTx not disabled
        WritePciConfigWord(DevExt, PCI_COMMAND_OFFSET, cmdReg);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - PCI Command Register = %04X (IntDis=%d)\n",
                       cmdReg, !!(cmdReg & PCI_INTERRUPT_DISABLE));
#endif
    }


    // Force legacy INTx on XP/2000 to avoid MSI/MSI-X related timeouts
    NvmeForceLegacyIntx(DevExt);

    // Read interrupt configuration from PCI config space
    // This is probably redundant on Win2K and can be ifdefed out
    {
        UCHAR interruptLine = ReadPciConfigByte(DevExt, PCI_INTERRUPT_LINE_OFFSET);
        UCHAR interruptPin = ReadPciConfigByte(DevExt, PCI_INTERRUPT_PIN_OFFSET);

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - PCI Interrupt Line=%d Pin=%d\n",
                       interruptLine, interruptPin);
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - ConfigInfo BEFORE: BusInterruptLevel=%d Vector=%d Mode=%d\n",
                       ConfigInfo->BusInterruptLevel, ConfigInfo->BusInterruptVector, ConfigInfo->InterruptMode);
#endif

#if (_WIN32_WINNT < 0x500)
        // CRITICAL FOR NT4: When manually setting SystemIoBusNumber/SlotNumber,
        // SCSI port doesn't automatically query interrupt configuration from HAL.
        // We MUST set valid interrupt parameters or we'll get STATUS_INVALID_PARAMETER.
        // For PCI: BusInterruptLevel = interrupt line, mode = LevelSensitive
        // NT 3.1 (no PCI slot, ISA/EISA bus): no interrupt unless "IRQ=1"; a
        // failed interrupt connection there fails the adapter, and with it the
        // boot. "IRQ=0" on any version: Fast Polling, no interrupt.
        if (!HasSlot && !ArgPresent(ArgumentString, "IRQ=1")) {
            interruptPin = 0;
        }
#ifdef NVMENT_W9X
        if (!ArgPresent(ArgumentString, "IRQ=1")) {
            interruptPin = 0;
        }
#endif
        if (ArgPresent(ArgumentString, "IRQ=0")) {
            interruptPin = 0;
        }
        if (interruptPin != 0 && interruptLine != 0 && interruptLine != 0xFF) {
            ConfigInfo->BusInterruptLevel = interruptLine;
            ConfigInfo->BusInterruptVector = interruptLine;
            ConfigInfo->InterruptMode = LevelSensitive;
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - Set interrupt from PCI config: Level=%d\n", interruptLine);
#endif
        } else {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - WARNING: No valid PCI interrupt configured\n");
#endif
            // If no valid interrupt, still need to set something valid for NT4.
            // Use polling mode - but this might not work on NT4!
            ConfigInfo->BusInterruptLevel = 0;
            ConfigInfo->BusInterruptVector = 0;
        }
#else
        // Win2000/XP: leave interrupt routing to ScsiPort/HAL; do not override ConfigInfo here.
#endif
        ConfigInfo->InterruptMode = LevelSensitive;
    }

    // Fast Polling (completions polled in HwStartIo) without an interrupt,
    // in the dump stack and with "IRQ=0"
    DevExt->DumpMode = (BOOLEAN)(g_DumpMode || ArgPresent(ArgumentString, "DUMP="));
    DevExt->ApstAllowed = ArgPresent(ArgumentString, "APST=1");
    DevExt->PollMode = (BOOLEAN)(DevExt->DumpMode || ArgPresent(ArgumentString, "IRQ=0") ||
                                 (ConfigInfo->BusInterruptLevel == 0 && ConfigInfo->BusInterruptVector == 0));

    // Set the number of access ranges we're using (1 for BAR0)
    // NT4 may require this to be explicitly set for resource translation
    ConfigInfo->NumberOfAccessRanges = 1;

    ConfigInfo->MaximumTransferLength = 32u << NVME_PAGE_SHIFT;  // safe default
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Master = TRUE;
    ConfigInfo->CachesData = TRUE;  // ScsiPort passes SRB_FUNCTION_SHUTDOWN and FLUSH only then
    ConfigInfo->AdapterScansDown = FALSE;
    ConfigInfo->Dma32BitAddresses = TRUE;
#if (_WIN32_WINNT >= 0x500)
    ConfigInfo->Dma64BitAddresses = TRUE;  // NVMe supports 64-bit addressing
#endif    
#ifdef NVMENT_NT31_COMPAT
    if (NVMENT_CONFIG_HAS(ConfigInfo, MaximumNumberOfTargets))
#endif
    ConfigInfo->MaximumNumberOfTargets = 2;  // Target 0: namespaces as LUNs; ScsiPort skips the initiator ID (1)
    ConfigInfo->InitiatorBusId[0] = 1;
    ConfigInfo->NumberOfPhysicalBreaks = 511;  // PRP1 + PRP list (512 entries)
    ConfigInfo->AlignmentMask = 0x3;  // DWORD alignment
    ConfigInfo->NeedPhysicalAddresses = TRUE;  // Required for ScsiPortGetPhysicalAddress to work
    ConfigInfo->TaggedQueuing = TRUE;  // Support tagged command queuing
    ConfigInfo->MultipleRequestPerLu = TRUE;  // Allow multiple outstanding commands per LUN
    ConfigInfo->AutoRequestSense = TRUE;  // Automatically provide sense data on errors
    // Note: SrbExtensionSize must be set in HW_INITIALIZATION_DATA, not here

#ifdef NVMENT_DBG_EXTRA
    {
        ULONG a;
        accessRange = &((*(ConfigInfo->AccessRanges))[0]);
        for (a = 0; a < ConfigInfo->NumberOfAccessRanges; a++) {
            ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - range:%u start:%08X, length:%08X, inMemory:%d\n",
                           a, (ULONG)(accessRange->RangeStart.QuadPart),
                           accessRange->RangeLength,
                           accessRange->RangeInMemory);
            accessRange++;
        }
    }
#endif
    // Determine BAR0 / controller register range.
    // On Win2000/XP PnP paths, ScsiPort usually provides a translated AccessRange already.
    // Only fall back to PCI BAR probing ("BAR sizing dance") if the AccessRange is not populated.
    accessRange = &((*(ConfigInfo->AccessRanges))[0]);

    if (accessRange->RangeLength != 0 && accessRange->RangeStart.QuadPart != 0) {
        // PnP path: trust the provided resource assignment.
        accessRange->RangeInMemory = TRUE; // NVMe register space is MMIO
        DevExt->ControllerRegistersLength = accessRange->RangeLength;
    } else {
        // Non-PnP / textmode scan path: read BAR0 from PCI config and size it.
        bar0tmp = ReadPciConfigDword(DevExt, PCI_BASE_ADDRESS_0);
        bar0.HighPart = 0;
        bar0.LowPart = bar0tmp & 0xFFFFFFF0;
        if ((bar0tmp & 0x6) == 0x6) {
            bar0.HighPart = ReadPciConfigDword(DevExt, PCI_BASE_ADDRESS_1);
            if (bar0.HighPart) {
                ScsiDebugPrint(0, "NVMENT: BAR0 base=0x%08X%08X beyond 4GB, it may not end well on 32bit kernel\n",
                            bar0.HighPart,
                            bar0.LowPart);
            }
        }

    #if (_WIN32_WINNT >= 0x500 && !defined(ALPHA))
        accessRange->RangeStart = ScsiPortConvertUlongToPhysicalAddress(ScsiPortConvertPhysicalAddressToULongPtr(bar0));
    #else
        accessRange->RangeStart = ScsiPortConvertUlongToPhysicalAddress(bar0.LowPart);
    #endif
        if (bar0tmp & 0x1) {
            return SP_RETURN_ERROR;
        }
        accessRange->RangeInMemory = TRUE;

        // Size BAR0 only if we had to probe it ourselves.
        WritePciConfigDword(DevExt, PCI_BASE_ADDRESS_0, 0xFFFFFFFF);

        // Read back the modified value
        barSize = ReadPciConfigDword(DevExt, PCI_BASE_ADDRESS_0);

        // Restore original BAR value
        WritePciConfigDword(DevExt, PCI_BASE_ADDRESS_0, bar0tmp);
        accessRange->RangeLength = ~(barSize & 0xFFFFFFF0) + 1;

        DevExt->ControllerRegistersLength = accessRange->RangeLength;
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - BAR0 base=0x%08X size=0x%08X\n",
                   (ULONG)accessRange->RangeStart.QuadPart,
                   accessRange->RangeLength);
#endif

    // Without a PCI slot (NT 3.1) the BAR is mapped on the bus ScsiPort called us for
    mapBus = ConfigInfo->SystemIoBusNumber;

#if !defined(NVMENT_NT31_COMPAT) && !defined(NVMENT_W9X)
    // Validate the access range (required for NT4, optional for Win2K+)
    // This checks if the range is available and doesn't conflict with other devices
    // (NT 3.1 has no ScsiPortValidateRange; ScsiPort checks the access range itself)
    if (!ScsiPortValidateRange(
            (PVOID)DevExt,
            ConfigInfo->AdapterInterfaceType,
            ConfigInfo->SystemIoBusNumber,
            accessRange->RangeStart,
            accessRange->RangeLength,
            (BOOLEAN)!accessRange->RangeInMemory)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - ScsiPortValidateRange failed for BAR0\n");
#endif
        return SP_RETURN_ERROR;
    }
#endif

    // Map the controller registers
#ifdef NVMENT_W9X
    (VOID)mapBus;
    DevExt->ControllerRegisters = NvmeMapPhysToLinear(accessRange->RangeStart.LowPart,
                                                      accessRange->RangeLength, 0);
    if (DevExt->ControllerRegisters == (PVOID)0xFFFFFFFF) {
        DevExt->ControllerRegisters = NULL;
    }
#else
    DevExt->ControllerRegisters = ScsiPortGetDeviceBase(
        (PVOID)DevExt,
        ConfigInfo->AdapterInterfaceType,
        mapBus,
        accessRange->RangeStart,
        accessRange->RangeLength,
        (BOOLEAN)!accessRange->RangeInMemory);
#endif

    if (DevExt->ControllerRegisters == NULL) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - failed to map controller registers\n");
#endif
        return SP_RETURN_ERROR;
    }
#if 0    
// can be reported in newer OSes
    // Read controller capabilities to determine max queue size
    // This must be done here so we can set ConfigInfo->NumberOfRequests
    {
        ULONGLONG cap;
        USHORT mqes, maxQueueSize;

        cap = NvmeReadReg64(DevExt, NVME_REG_CAP);
        mqes = (USHORT)(cap & NVME_CAP_MQES_MASK);
        maxQueueSize = mqes + 1;  // MQES is 0-based

        // Limit to our maximum (what fits in one page)
        if (maxQueueSize > NVME_MAX_QUEUE_SIZE) {
            maxQueueSize = NVME_MAX_QUEUE_SIZE;
        }

        // Tell SCSI port driver how many outstanding requests we can handle
        // This prevents the OS from sending more requests than we can queue
        ConfigInfo->NumberOfRequests = maxQueueSize;

        ScsiDebugPrint(0, "NVMENT: HwFindAdapter - MQES=%u, setting NumberOfRequests=%u\n",
                       mqes, maxQueueSize);
    }
#endif

//
// Uncached memory size calculation:
// - Admin SQ: 4096 bytes (4KB aligned)
// - I/O SQ: 4096 bytes (4KB aligned)
// - Utility buffer / PRP list pool: (SgListPages pages * 4KB, page-aligned)
// - Admin CQ: 4096 bytes (4KB aligned)
// - I/O CQ: 4096 bytes (4KB aligned)
// Total: ~60KB with alignment
//

#ifdef NVMENT_NT31_COMPAT
    if (!HasSlot) {
        // NT 3.1 HAL: an ISA bus master always gets map registers below 16 MB,
        // and the copy-back from them overwrites data the driver wrote itself
        // into the request buffer. A scatter/gather master on EISA gets real
        // physical addresses. Must be set before the uncached extension is taken.
        ConfigInfo->AdapterInterfaceType = Eisa;
    }
#endif

    // Allocate uncached memory block:
    // queues (4 pages), PRP list pool / utility buffer, TRIM pattern, alignment
    // The dump stack has little memory and does one request at a time.
    DevExt->SgListPages = DevExt->DumpMode ? 4 : 32;
    DevExt->UncachedExtensionSize = (NVME_PAGE_SIZE * (DevExt->SgListPages + 4 + 1 + 1));

    DevExt->UncachedExtensionBase = ScsiPortGetUncachedExtension(
        (PVOID)DevExt,
        ConfigInfo,
        DevExt->UncachedExtensionSize);

    if (DevExt->UncachedExtensionBase == NULL) {
        DevExt->SgListPages = DevExt->DumpMode ? 2 : 16;
        DevExt->UncachedExtensionSize = (NVME_PAGE_SIZE * (DevExt->SgListPages + 4 + 1 + 1));

        DevExt->UncachedExtensionBase = ScsiPortGetUncachedExtension(
            (PVOID)DevExt,
            ConfigInfo,
            DevExt->UncachedExtensionSize);

        if (DevExt->UncachedExtensionBase == NULL) {
    #ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - failed to allocate uncached memory\n");
    #endif
            return SP_RETURN_ERROR;
        }
    }

    // Get physical address of the entire uncached block
    tempSize = DevExt->UncachedExtensionSize;
    DevExt->UncachedExtensionPhys = ScsiPortGetPhysicalAddress(
        (PVOID)DevExt,
        NULL,
        DevExt->UncachedExtensionBase,
        &tempSize);
    
    // Zero out the entire uncached memory
    memset(DevExt->UncachedExtensionBase, 0, DevExt->UncachedExtensionSize);

    // Initialize allocator
    DevExt->UncachedExtensionOffset = 0;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - allocated %u bytes of uncached memory at virt=%p phys=%08X%08X\n",
                   DevExt->UncachedExtensionSize, DevExt->UncachedExtensionBase,
                   (ULONG)(DevExt->UncachedExtensionPhys.QuadPart >> 32),
                   (ULONG)(DevExt->UncachedExtensionPhys.QuadPart & 0xFFFFFFFF));
#endif

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - success, returning SP_RETURN_FOUND\n");
#endif

    if (!NvmeSanitizeController(DevExt)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwInHwFoundAdapteritialize - NvmeSanitizeController failed\n");
#endif
        return SP_RETURN_ERROR;
    }

    if (!NvmeInitializeController(DevExt)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - NvmeInitializeController failed\n");
#endif
        return SP_RETURN_ERROR;
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFoundAdapter - %s, IRQ level %u\n",
                   DevExt->PollMode ? "Fast Polling" : "interrupts", ConfigInfo->BusInterruptLevel);
#endif

    // We need to update some of the config info now from the data we got from the controller.
    ConfigInfo->MaximumTransferLength = DevExt->MaxTransferSizeBytes;
    if (((ConfigInfo->MaximumTransferLength >> NVME_PAGE_SHIFT) - 1) < ConfigInfo->NumberOfPhysicalBreaks)
        ConfigInfo->NumberOfPhysicalBreaks = (ConfigInfo->MaximumTransferLength >> NVME_PAGE_SHIFT) - 1;

    return SP_RETURN_FOUND;
}

//
// HwFindAdapter - Locate and configure the adapter
//
ULONG HwFindAdapter(
    IN PVOID DeviceExtension,
    IN PVOID HwContext,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;
    ULONG pciBuffer[64];
    ULONG slotNumber;
    ULONG busNumber;
    UCHAR baseClass, subClass, progIf;
    ULONG bytesRead;
    BOOLEAN PNP = ConfigInfo->NumberOfAccessRanges != 0;
    BOOLEAN hasSlot = TRUE;

#ifdef NVMENT_NT31_COMPAT
    g_FindAdapterCalled = TRUE;
    PciProbeAccess(DevExt);
    // NT 3.1: no SlotNumber, called for the ISA bus rather than a PCI slot
    hasSlot = (BOOLEAN)NVMENT_CONFIG_HAS(ConfigInfo, SlotNumber);
    if (!hasSlot) {
        PNP = FALSE;
    }
#endif

    DevExt->InterruptCount = 0;

#ifdef NVMENT_W9X
    // SCSIPORT.PDR calls once per device node: the function it names when
    // that is an NVMe controller nobody took yet, else the next free one
    *Again = FALSE;
    busNumber = ConfigInfo->SystemIoBusNumber;
    slotNumber = ConfigInfo->SlotNumber;
    bytesRead = GetPciConfig(DevExt, busNumber, slotNumber, pciBuffer, 256);
    if (bytesRead == 0 || NvmeIsClaimed(busNumber, slotNumber) ||
        !IsNvmeDevice(((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 2], ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 1],
                      ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET])) {
        BOOLEAN found = FALSE;
        for (busNumber = 0; busNumber < 16 && !found; busNumber++) {
            for (slotNumber = 0; slotNumber < PCI_MAX_DEVICES * PCI_MAX_FUNCTION; slotNumber++) {
                if (GetPciConfig(DevExt, busNumber, slotNumber, pciBuffer, 256) == 0) continue;
                if (*(USHORT*)&((PUCHAR)pciBuffer)[PCI_VENDOR_ID_OFFSET] == 0xFFFF) continue;
                if (NvmeIsClaimed(busNumber, slotNumber)) continue;
                if (IsNvmeDevice(((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 2],
                                 ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 1],
                                 ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET])) {
                    found = TRUE;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) {
            return SP_RETURN_NOT_FOUND;
        }
    }
    DevExt->VendorId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_VENDOR_ID_OFFSET];
    DevExt->DeviceId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_DEVICE_ID_OFFSET];
    DevExt->RevisionId = ((PUCHAR)pciBuffer)[PCI_REVISION_ID_OFFSET];
    DevExt->BusNumber = busNumber;
    DevExt->SlotNumber = slotNumber;
    NvmeClaim(busNumber, slotNumber);
    ConfigInfo->AdapterInterfaceType = PCIBus;
    ConfigInfo->SystemIoBusNumber = busNumber;
    ConfigInfo->SlotNumber = slotNumber;
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwFindAdapter (9x) - bus %d slot %d: VID=%04X DID=%04X\n",
                   busNumber, slotNumber, DevExt->VendorId, DevExt->DeviceId);
#endif
    return HwFoundAdapter(DevExt, ConfigInfo, (PUCHAR)pciBuffer, ArgumentString, TRUE);
#endif

    if (HwContext) {
        busNumber = ((PULONG)HwContext)[0];
        slotNumber = ((PULONG)HwContext)[1];
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFindAdapter:%p called with HwContext - Bus=%d Slot=%d\n",
                       DeviceExtension, busNumber, slotNumber);
#endif
    } else {
        // Win2k, PnP gives us this directly
        busNumber = ConfigInfo->SystemIoBusNumber;
        slotNumber = ConfigInfo->SlotNumber;
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFindAdapter:%p called w/o HwContext - Bus=%d Slot=%d PNP=%d\n",
                       DeviceExtension, busNumber, slotNumber, PNP);
#endif
    }

    // PnP path (Win2000/XP): ScsiPort gives us the exact bus/slot to probe.
    // Do NOT scan other slots here, or you may confuse enumeration and waste time in textmode.
    if (!HwContext && PNP) {
        bytesRead = GetPciConfig(DevExt, busNumber, slotNumber, pciBuffer, 256);

        if (bytesRead == 0) {
            *Again = FALSE;
            return SP_RETURN_NOT_FOUND;
        }

        DevExt->VendorId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_VENDOR_ID_OFFSET];
        DevExt->DeviceId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_DEVICE_ID_OFFSET];

        if (DevExt->VendorId == 0xFFFF || DevExt->VendorId == 0x0000) {
            *Again = FALSE;
            return SP_RETURN_NOT_FOUND;
        }

        DevExt->RevisionId = ((PUCHAR)pciBuffer)[PCI_REVISION_ID_OFFSET];
        progIf = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET];
        subClass = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 1];
        baseClass = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 2];

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFindAdapter (PnP) - bus %d slot %d: VID=%04X DID=%04X Class=%02X%02X%02X\n",
                       busNumber, slotNumber, DevExt->VendorId, DevExt->DeviceId,
                       baseClass, subClass, progIf);
#endif

        if (!IsNvmeDevice(baseClass, subClass, progIf)) {
            *Again = FALSE;
            return SP_RETURN_NOT_FOUND;
        }

        DevExt->BusNumber = busNumber;
        DevExt->SlotNumber = slotNumber;
        *Again = FALSE;
        
        // Ensure ConfigInfo matches the adapter we found (needed for non-PnP/textmode/NT4 scan paths)
        ConfigInfo->AdapterInterfaceType = PCIBus;
        ConfigInfo->SystemIoBusNumber = busNumber;
        ConfigInfo->SlotNumber = slotNumber;
        return HwFoundAdapter(DevExt, ConfigInfo, (PUCHAR)pciBuffer, ArgumentString, TRUE);
    }

scanloop:
    // Read PCI configuration space for THIS slot only
    bytesRead = GetPciConfig(DevExt, busNumber, slotNumber, pciBuffer, 256);

    if (bytesRead == 0) {
        // No device in this slot - tell SCSI port to keep scanning other slots
        goto scannext;
    }

    // Extract Vendor ID and Device ID
    DevExt->VendorId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_VENDOR_ID_OFFSET];
    DevExt->DeviceId = *(USHORT*)&((PUCHAR)pciBuffer)[PCI_DEVICE_ID_OFFSET];

    // Check for invalid vendor ID
    if (DevExt->VendorId == 0xFFFF || DevExt->VendorId == 0x0000) {
        // No valid device in this slot - tell SCSI port to keep scanning other slots
        goto scannext;
    }

    // Functions 1-7 only exist on multi-function devices (some chipsets
    // answer for every function of a single-function device). Linear slot
    // numbers: device in bits 4:0, function in bits 7:5.
    if ((slotNumber >> 5) != 0) {
        ULONG header0[4];
        if (GetPciConfig(DevExt, busNumber, slotNumber & 31, header0, sizeof(header0)) < sizeof(header0) ||
            !(((PUCHAR)header0)[PCI_HEADER_TYPE_OFFSET] & 0x80)) {
            goto scannext;
        }
    }

    // Extract class code information
    DevExt->RevisionId = ((PUCHAR)pciBuffer)[PCI_REVISION_ID_OFFSET];
    progIf = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET];
    subClass = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 1];
    baseClass = ((PUCHAR)pciBuffer)[PCI_CLASS_CODE_OFFSET + 2];

#ifdef NVMENT_DBG_EXTRA
    ScsiDebugPrint(0, "NVMENT: HwFindAdapter - bus %d slot %d: VID=%04X DID=%04X Class=%02X%02X%02X\n",
                   busNumber, slotNumber, DevExt->VendorId, DevExt->DeviceId,
                   baseClass, subClass, progIf);
#endif

    // Check if this is an NVMe device
    if (IsNvmeDevice(baseClass, subClass, progIf)) {
        // Found an NVMe device at the slot SCSI port asked us to check!
        DevExt->BusNumber = busNumber;
        DevExt->SlotNumber = slotNumber;

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwFindAdapter - bus %d slot %d: VID=%04X DID=%04X\n",
                       busNumber, slotNumber, DevExt->VendorId, DevExt->DeviceId);
#endif
        // Ensure ConfigInfo matches the adapter we found (needed for non-PnP/textmode/NT4 scan paths)
        if (hasSlot) {
            ConfigInfo->AdapterInterfaceType = PCIBus;
            ConfigInfo->SystemIoBusNumber = busNumber;
            ConfigInfo->SlotNumber = slotNumber;
        }
        // Store next slot/bus so we can resume scanning on next call
        if (HwContext) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwFindAdapter - NT4: store HwContext to resume scanning\n");
#endif
            ((PULONG)HwContext)[0] = DevExt->BusNumber;
            ((PULONG)HwContext)[1] = DevExt->SlotNumber + 1;
            if (((PULONG)HwContext)[1] >= (PCI_MAX_DEVICES*PCI_MAX_FUNCTION)) {
                ((PULONG)HwContext)[1] = 0;
                ((PULONG)HwContext)[0]++;
            }
            *Again = TRUE;
        } else {
            *Again = FALSE;
        }
#ifdef NVMENT_DBG
        if (!HwContext) {
            ScsiDebugPrint(0, "NVMENT: HwFindAdapter - uh oh no HwContext! are we going to scan whole thing again?\n");
        }
#endif
        return HwFoundAdapter(DevExt, ConfigInfo, (PUCHAR)pciBuffer, ArgumentString, hasSlot);
    }

scannext:
    // NT4 or install time: Continue scanning the bus
    // (linear slot numbers: device in bits 4:0, function in bits 7:5)
    slotNumber++;
    if (slotNumber >= (PCI_MAX_DEVICES*PCI_MAX_FUNCTION)) {
        busNumber++;
        slotNumber = 0;
        if (busNumber == 16) { // theoretically 256 but we arent going to waste cycles
            *Again = FALSE;
            return SP_RETURN_NOT_FOUND;
        }
    }
    goto scanloop;
}

//
// HwInitialize - Initialize the adapter
//
BOOLEAN HwInitialize(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwInitialize called\n");
#endif

    // Ensure legacy INTx is not disabled at PCI command level.
    // Some ScsiPort environments (especially textmode) may not use MSI/MSI-X reliably.
    {
        USHORT cmdReg = ReadPciConfigWord(DevExt, PCI_COMMAND_OFFSET);
        if (cmdReg & PCI_INTERRUPT_DISABLE) {
            cmdReg = (USHORT)(cmdReg & ~PCI_INTERRUPT_DISABLE);
            WritePciConfigWord(DevExt, PCI_COMMAND_OFFSET, cmdReg);
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwInitialize - cleared PCI_INTERRUPT_DISABLE (Cmd=%04X)\n", cmdReg);
#endif
        }
    }

    // Step 3: Enable interrupts
    // This is done after initialization is complete but before HwInitialize returns,
    // so ScsiPort knows we're interrupt-capable. Fast Polling keeps them masked.
    if (!DevExt->PollMode) {
        NvmeEnableInterrupts(DevExt);
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwInitialize finished successfully\n");
#endif
    return TRUE;
}

//
// HwStartIo - Process SCSI request
//
static BOOLEAN NvmeStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);

BOOLEAN HwStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;
    BOOLEAN driverIoctl = FALSE;
    BOOLEAN result;

    // "NVMENT" IOCTLs (info, power down/up) do not wake the controller up
    if (Srb->Function == SRB_FUNCTION_IO_CONTROL && Srb->DataBuffer != NULL &&
        Srb->DataTransferLength >= sizeof(SRB_IO_CONTROL) &&
        NvmeIsOwnSignature(((PSRB_IO_CONTROL)Srb->DataBuffer)->Signature)) {
        driverIoctl = TRUE;
    }

    // System shutdown: shutdown notification, so the controller saves its
    // cache and tables before the power goes (an unsafe shutdown otherwise).
    // A request after it sets the controller up again.
    if (Srb->Function == SRB_FUNCTION_SHUTDOWN) {
        if (DevExt->InitComplete && !DevExt->Stopped) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SRB_FUNCTION_SHUTDOWN - shutdown notification\n");
#endif
            NvmePowerDown(DevExt);
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
        ScsiPortNotification(NextRequest, DeviceExtension, NULL);
        return TRUE;
    }

    // Shut down (power down), or the controller lost its state (sleep state,
    // BIOS, another driver): set it up again first
    if (!driverIoctl && Srb->PathId == 0 && Srb->TargetId == 0) {
        DevExt->RequestsFlushed = FALSE;
        if (!NvmeCheckState(DevExt)) {
            if (DevExt->RequestsFlushed) {
                return TRUE;            // completed with SRB_STATUS_BUS_RESET
            }
            Srb->SrbStatus = SRB_STATUS_ERROR;
            ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
            ScsiPortNotification(NextRequest, DeviceExtension, NULL);
            return TRUE;
        }
        if (DevExt->RequestsFlushed) {
            return TRUE;                // ScsiPort retries it
        }
    }

    result = NvmeStartIo(DeviceExtension, Srb);

    // Fast Polling: wait here for the completion
    if (DevExt->PollMode && Srb->SrbStatus == SRB_STATUS_PENDING) {
        NvmePollSrb(DevExt, Srb);
    }
    return result;
}

static BOOLEAN NvmeStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;

#ifdef NVMENT_DBG_EXTRA
    ScsiDebugPrint(0, "NVMENT: HwStartIo called - Function=%02X Path=%d Target=%d Lun=%d\n",
                   Srb->Function, Srb->PathId, Srb->TargetId, Srb->Lun);
#endif

#if 1
    // Poll completion queues as a backup in case interrupts are delayed
    // This helps performance on busy systems
    if (DevExt->InitComplete && DevExt->CurrentQueueDepth > 0) {
        NvmeProcessAdminCompletion(DevExt);
        NvmeProcessIoCompletion(DevExt);
    }
#endif

    // Target 0 only; LUN n is the n-th namespace. A controller without
    // namespaces presents no logical unit at all.
    if (Srb->PathId != 0 || Srb->TargetId != 0 || Srb->Lun >= NVMENT_MAX_LUNS ||
        Srb->Lun >= DevExt->LunCount) {
        if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI || Srb->Function == SRB_FUNCTION_IO_CONTROL) {
            Srb->SrbStatus = (UCHAR)((Srb->PathId == 0 && Srb->TargetId == 0 && DevExt->LunCount != 0) ?
                                     SRB_STATUS_INVALID_LUN : SRB_STATUS_SELECTION_TIMEOUT);
        } else {
            Srb->SrbStatus = SRB_STATUS_SUCCESS;    // flush/shutdown/reset: nothing there
        }
        ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
        ScsiPortNotification(NextRequest, DeviceExtension, NULL);
        return TRUE;
    }

    // Sense kept for REQUEST SENSE belongs to the request before only
    if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI && Srb->Cdb[0] != SCSIOP_REQUEST_SENSE) {
        DevExt->Luns[Srb->Lun].SensePending = FALSE;
    }

    // Process the SRB based on its function
    switch (Srb->Function) {
        case SRB_FUNCTION_EXECUTE_SCSI:
#ifdef NVMENT_DBG_EXTRA
            ScsiDebugPrint(0, "NVMENT: processing op=%02X\n", Srb->Cdb[0]);

            // Log tagged queuing usage
            if (Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) {
                const char* queueType = "Unknown";
                switch (Srb->QueueAction) {
                    case SRB_SIMPLE_TAG_REQUEST: queueType = "SIMPLE"; break;
                    case SRB_HEAD_OF_QUEUE_TAG_REQUEST: queueType = "HEAD_OF_QUEUE"; break;
                    case SRB_ORDERED_QUEUE_TAG_REQUEST: queueType = "ORDERED"; break;
                }
                ScsiDebugPrint(0, "NVMENT: Tagged queuing enabled - QueueAction=%s (0x%02X) Tag:%02X\n",
                               queueType, Srb->QueueAction, Srb->QueueTag);
            }
#endif
            // Process SCSI CDB
            switch (Srb->Cdb[0]) {
                case SCSIOP_READ6:
                case SCSIOP_READ:
                case SCSIOP_READ12:
                case SCSIOP_READ16:
                case SCSIOP_WRITE6:
                case SCSIOP_WRITE:
                case SCSIOP_WRITE12:
                case SCSIOP_WRITE16:
                    return ScsiHandleReadWrite(DevExt, Srb);
                    
                case SCSIOP_TEST_UNIT_READY:
                    if (DevExt->InitComplete)
                        Srb->SrbStatus = SRB_STATUS_SUCCESS;
                    else
                        Srb->SrbStatus = SRB_STATUS_BUSY;
                    break;

                case SCSIOP_VERIFY6:
                    Srb->SrbStatus = SRB_STATUS_SUCCESS;
                    break;

                case SCSIOP_VERIFY:
                case SCSIOP_VERIFY12:
                case SCSIOP_VERIFY16:
                    return ScsiHandleVerify(DevExt, Srb);

                case SCSIOP_UNMAP:
                    return ScsiHandleUnmap(DevExt, Srb);

                case SCSIOP_REQUEST_SENSE:
                    return ScsiHandleRequestSense(DevExt, Srb);

                case SCSIOP_MODE_SELECT:
                case SCSIOP_MODE_SELECT10:
                    return ScsiHandleModeSelect(DevExt, Srb);

                case SCSIOP_REPORT_LUNS:
                    return ScsiHandleReportLuns(DevExt, Srb);
                    
                case SCSIOP_INQUIRY:
                    return ScsiHandleInquiry(DevExt, Srb);

                case SCSIOP_READ_CAPACITY:
                    return ScsiHandleReadCapacity(DevExt, Srb);

                case SCSIOP_READ_CAPACITY16:
                    return ScsiHandleReadCapacity16(DevExt, Srb);

                case SCSIOP_LOG_SENSE:
                    return ScsiHandleLogSense(DevExt, Srb);

                case SCSIOP_MODE_SENSE:
                case SCSIOP_MODE_SENSE10:
                    return ScsiHandleModeSense(DevExt, Srb);
                    
                case SCSIOP_START_STOP_UNIT:
                    return ScsiHandleStartStop(DevExt, Srb);
                    
                case SCSIOP_SYNCHRONIZE_CACHE:
                case SCSIOP_SYNCHRONIZE_CACHE16:
                    return ScsiHandleFlush(DevExt, Srb);

                case SCSIOP_ATA_PASSTHROUGH16:
                case SCSIOP_ATA_PASSTHROUGH12:
                    // SAT (SCSI/ATA Translation) ATA PASS-THROUGH commands
                    return ScsiHandleSatPassthrough(DevExt, Srb);

                case SCSIOP_READ_DEFECT_DATA10:
                    return ScsiHandleReadDefectData10(DevExt, Srb);

                default:
#ifdef NVMENT_DBG
                    ScsiDebugPrint(0, "NVMENT: HwStartIo - unimplemented SCSI opcode 0x%02X\n", Srb->Cdb[0]);
#endif
                    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
                    break;
            }
            break;

        case SRB_FUNCTION_FLUSH:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo SRB_FUNCTION_FLUSH\n");
#endif
            return ScsiHandleFlush(DevExt, Srb);

        case SRB_FUNCTION_FLUSH_QUEUE:
            // No internal queue to flush - requests go directly to hardware
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_SHUTDOWN:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo - SRB_FUNCTION_SHUTDOWN - flushing cache\n");
#endif
            // Flush all cached writes before shutdown
            return ScsiHandleFlush(DevExt, Srb);

        case SRB_FUNCTION_ABORT_COMMAND:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo SRB_FUNCTION_ABORT_COMMAND\n");
#endif
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_RESET_BUS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo SRB_FUNCTION_RESET_BUS\n");
#endif
            HwResetBus(DevExt, Srb->PathId);
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_RESET_DEVICE:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo SRB_FUNCTION_RESET_DEVICE\n");
#endif
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_IO_CONTROL:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo - SRB_FUNCTION_IO_CONTROL\n");
#endif
            if (Srb->DataTransferLength >= sizeof(SRB_IO_CONTROL)) {
                PSRB_IO_CONTROL srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
                if (NvmeIsOwnSignature(srbControl->Signature)) {
                    if (HandleIO_NVMENT(DevExt, Srb)) {
                        if (Srb->SrbStatus != SRB_STATUS_PENDING) {
                            Srb->SrbStatus = SRB_STATUS_SUCCESS;
                        }
                    } else {
                        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;        
                    }
                } else if (memcmp(srbControl->Signature, "NvmeMini", 8) == 0) {
                    if (HandleIO_NvmeMini(DevExt, Srb)) {
                        if (Srb->SrbStatus != SRB_STATUS_PENDING) {
                            Srb->SrbStatus = SRB_STATUS_SUCCESS;
                        }
                    } else {
                        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;        
                    }
                } else if (memcmp(srbControl->Signature, "SCSIDISK", 8) == 0) {
                    if (HandleIO_SCSIDISK(DevExt, Srb)) {
                        // If HandleIO_SCSIDISK returns TRUE, it may have set the status
                        // to PENDING for async operations.
                        if (Srb->SrbStatus != SRB_STATUS_PENDING) {
                            Srb->SrbStatus = SRB_STATUS_SUCCESS;
                        }
                    } else {
                        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;        
                    }
                } else {
                    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;    
                }
            } else {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: HwStartIo - SRB_FUNCTION_IO_CONTROL invalid transfer length - DataTransferLength=%u\n",
                            Srb->DataTransferLength);
#endif
                Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
                return FALSE;
            }
            break;

        default:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: HwStartIo - unimplemented SRB function 0x%02X\n", Srb->Function);
#endif
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            break;
    }

    // Complete the request if not pending
    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
        ScsiPortNotification(NextRequest, DeviceExtension, NULL);
    }

    return TRUE;
}

//
// NvmeWatchdog - While commands are outstanding, check the completion queues
// every 100 ms. Completions found here were not reported by an interrupt;
// when that keeps happening the interrupt is not delivered (wrong routing,
// MSI-only device, text-mode setup) and the driver continues with Fast Polling.
//
static VOID NvmeWatchdog(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;
    BOOLEAN found;

    DevExt->TimerArmed = FALSE;
    if (DevExt->PollMode || DevExt->Stopped || !DevExt->InitComplete) {
        return;
    }

    found = NvmeProcessAdminCompletion(DevExt);
    if (NvmeProcessIoCompletion(DevExt)) {
        found = TRUE;
    }
    if (found && ++DevExt->MissedInterrupts >= NVMENT_MISSED_IRQ_LIMIT) {
        NvmeEnterPollMode(DevExt);
        return;
    }
    if (DevExt->CurrentQueueDepth != 0) {
        NvmeArmWatchdog(DevExt);
    }
}

VOID NvmeArmWatchdog(IN PHW_DEVICE_EXTENSION DevExt)
{
    if (DevExt->PollMode || DevExt->TimerArmed || !DevExt->InitComplete) {
        return;
    }
    DevExt->TimerArmed = TRUE;
    ScsiPortNotification(RequestTimerCall, (PVOID)DevExt, NvmeWatchdog, NVMENT_WATCHDOG_USEC);
}

//
// NvmeEnterPollMode - Interrupts are not delivered: mask them, finish what is
// outstanding and use Fast Polling from now on
//
VOID NvmeEnterPollMode(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG wait = 0;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: interrupts not delivered, back to Fast Polling\n");
#endif
    DevExt->PollMode = TRUE;
    NvmeWriteReg32(DevExt, NVME_REG_INTMS, 0xFFFFFFFF);
    while (DevExt->CurrentQueueDepth != 0 && wait++ < 1000000) {
        NvmeProcessAdminCompletion(DevExt);
        NvmeProcessIoCompletion(DevExt);
        ScsiPortStallExecution(NVMENT_POLL_INTERVAL_USEC);
    }
}

//
// NvmePollSrb - Fast Polling: poll the queues until Srb completes, up to its
// timeout minus one second (ScsiPort's own timeout then resets the bus)
//
VOID NvmePollSrb(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG timeoutUsec;
    ULONG elapsed = 0;

    timeoutUsec = Srb->TimeOutValue ? Srb->TimeOutValue : 10;
    if (timeoutUsec > 1) {
        timeoutUsec--;
    }
    if (timeoutUsec > 60) {
        timeoutUsec = 60;
    }
    timeoutUsec *= 1000000;

    while (Srb->SrbStatus == SRB_STATUS_PENDING && elapsed < timeoutUsec) {
        NvmeProcessIoCompletion(DevExt);
        NvmeProcessAdminCompletion(DevExt);
        if (Srb->SrbStatus != SRB_STATUS_PENDING) {
            break;
        }
        ScsiPortStallExecution(NVMENT_POLL_INTERVAL_USEC);
        elapsed += NVMENT_POLL_INTERVAL_USEC;
    }
}

//
// HwInterrupt - ISR for adapter
//
BOOLEAN HwInterrupt(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;
    BOOLEAN interruptHandled = FALSE;

    if (!DevExt->ResourcesAllocated || DevExt->Stopped) {
        return FALSE;
    }

    // Process Admin Queue completions first
    if (NvmeProcessAdminCompletion(DevExt)) {
        interruptHandled = TRUE;
    }

    // Process I/O Queue completions
    if (NvmeProcessIoCompletion(DevExt)) {
        interruptHandled = TRUE;
    }

    // Only treat this as a "real" interrupt if we actually consumed NVMe completions.
    // This avoids clearing the watchdog's count on shared/spurious IRQs.
    if (interruptHandled) {
        DevExt->InterruptCount++;
        DevExt->MissedInterrupts = 0;
    }

    return interruptHandled;
}

//
// HwResetBus - Reset the SCSI bus
//
BOOLEAN HwResetBus(IN PVOID DeviceExtension, IN ULONG PathId)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwResetBus - queue depth %u\n", DevExt->CurrentQueueDepth);
#endif

    // ScsiPort resets the bus when a request timed out. A command the
    // controller still holds would complete later into a request ScsiPort
    // already gave back (or into its retry): reset the controller, which
    // aborts everything, and set it up again.
    NvmeReinitialize(DevExt);

    // Complete all outstanding requests
    ScsiPortCompleteRequest(DeviceExtension, (UCHAR)PathId, 
                           SP_UNTAGGED, SP_UNTAGGED,
                           SRB_STATUS_BUS_RESET);

    return TRUE;
}

#if (_WIN32_WINNT >= 0x500)
//
// HwAdapterControl - Handle adapter power and PnP events (Windows 2000+)
//
SCSI_ADAPTER_CONTROL_STATUS HwAdapterControl(
    IN PVOID DeviceExtension,
    IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
    IN PVOID Parameters)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;
    SCSI_ADAPTER_CONTROL_STATUS status = ScsiAdapterControlSuccess;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwAdapterControl called - ControlType=%d\n", ControlType);
#endif
    switch (ControlType) {
        case ScsiQuerySupportedControlTypes:
            {
                PSCSI_SUPPORTED_CONTROL_TYPE_LIST list =
                    (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;

                // Indicate which control types we support
                list->SupportedTypeList[ScsiStopAdapter] = TRUE;
                list->SupportedTypeList[ScsiRestartAdapter] = TRUE;
            }
            break;

        case ScsiStopAdapter:
            // Stop the adapter - perform clean shutdown
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: ScsiStopAdapter - performing shutdown\n");
#endif
            NvmeShutdownController(DevExt);
            break;

        case ScsiRestartAdapter:
            // Restart the adapter - reinitialize controller
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: ScsiRestartAdapter - reinitializing\n");
#endif
            // Controller was shut down (and maybe powered off): set it up again
            if (NvmeReinitialize(DevExt)) {
                status = ScsiAdapterControlSuccess;
            } else {
                status = ScsiAdapterControlUnsuccessful;
            }
            break;

        default:
            status = ScsiAdapterControlUnsuccessful;
            break;
    }

    return status;
}
#else
//
// HwAdapterState - Handle adapter state changes (Windows NT 4)
//
BOOLEAN HwAdapterState(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN BOOLEAN SaveState)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)DeviceExtension;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HwAdapterState called - SaveState=%d\n", SaveState);
#endif

    if (SaveState) {
        // Save adapter state - prepare for power down or hibernation
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwAdapterState - saving state and shutting down\n");
#endif
        // Perform clean shutdown of the NVMe controller
        NvmeShutdownController(DevExt);
    } else {
        // Restore adapter state - reinitialize after power up
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HwAdapterState - restoring state\n");
#endif
        // Reinitialize the controller (also done by the next request)
        if (DevExt->Stopped) {
            return NvmeReinitialize(DevExt);
        }
    }
    return TRUE;
}
#endif
