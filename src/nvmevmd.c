/*
 * NVMENT - NVMe controllers behind an Intel Volume Management Device (VMD)
 *
 * With VMD on (Intel RST "VMD controller", 11th generation Core and later,
 * Xeon Scalable), the root ports of the NVMe slots and the controllers
 * behind them leave the PCI hierarchy the firmware and the HAL know. They
 * form a PCI domain of their own behind one endpoint of class 01:04:
 *   BAR0 (CFGBAR)   ECAM-style configuration space of the domain
 *                   (1 MB per bus, 4 KB per function)
 *   BAR2 (MEMBAR1)  memory window: the BARs of the domain's devices
 *   BAR4 (MEMBAR2)  second memory window, from offset 2000h (the VMD's own
 *                   MSI-X table comes first)
 *   40h VMCAP       bit 0: the domain's buses are restricted
 *   44h VMCONFIG    bits 9:8: first bus 0, 128 or 224; bit 1: MSI remapping off
 *   70h VMLOCK      bit 1: MEMBAR2 shadow registers (pass-through guests)
 * The domain's devices have no INTx (the VMD routes MSI/MSI-X only, which
 * no ScsiPort here uses): the controller runs in Fast Polling.
 * The firmware may or may not have set the domain up (a UEFI firmware with
 * an RST driver does, a legacy BIOS does not): bus numbers and memory
 * windows the controllers can be reached through are kept, otherwise the
 * whole domain is assigned again, the same way every time: buses in
 * depth-first order from the first bus, BARs upwards from MEMBAR1, bridge
 * windows 1 MB aligned.
 * The domain itself: vmdcore.h (shared with AHCINT).
 * After Linux drivers/pci/controller/vmd.c.
 */

#define VMD_CORE_IMPLEMENTATION
#include "nvment.h"

#ifdef NVMENT_W9X
PVOID __cdecl NvmeMapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG Flags);
#endif

/* An Intel VMD (its first 64 bytes of configuration space) */
BOOLEAN
NvmeIsVmd(IN PUCHAR PciConfig)
{
    return VmdIsVmd(*(PUSHORT)&PciConfig[PCI_VENDOR_ID_OFFSET], *(PUSHORT)&PciConfig[PCI_DEVICE_ID_OFFSET],
                    PciConfig[PCI_CLASS_CODE_OFFSET + 2]);
}

static PVOID
NvmeVmdMap(IN PVOID Context, IN ULONGLONG Phys, IN ULONG Length)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)Context;
#ifdef NVMENT_W9X
    PVOID va;
    (VOID)DevExt;
    if ((Phys >> 32) != 0) return NULL;
    va = NvmeMapPhysToLinear((ULONG)Phys, Length, 0);
    return va == (PVOID)0xFFFFFFFF ? NULL : va;
#else
    SCSI_PHYSICAL_ADDRESS pa;
#if !defined(_AMD64_)
    if ((Phys >> 32) != 0) return NULL;
#endif
    pa.QuadPart = (LONGLONG)Phys;
#ifdef NVMENT_DBG
    {
        PVOID va = ScsiPortGetDeviceBase((PVOID)DevExt, DevExt->VmdMapType, DevExt->VmdMapBus, pa, Length, FALSE);
        ScsiDebugPrint(0, "NVMENT: VMD map %08X+%X type %u bus %u -> %p\n", (ULONG)Phys, Length,
                       DevExt->VmdMapType, DevExt->VmdMapBus, va);
        return va;
    }
#else
    return ScsiPortGetDeviceBase((PVOID)DevExt, DevExt->VmdMapType, DevExt->VmdMapBus, pa, Length, FALSE);
#endif
#endif
}

/* The VMD's own configuration space (on the host bus) */
static ULONG
NvmeVmdHostRead(IN PVOID Context, IN ULONG Offset)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)Context;
    PUCHAR save = DevExt->VmdChildCfg;
    ULONG bus = DevExt->BusNumber, slot = DevExt->SlotNumber, v;
    DevExt->VmdChildCfg = NULL;
    DevExt->BusNumber = DevExt->VmdBus;
    DevExt->SlotNumber = DevExt->VmdSlot;
    v = ReadPciConfigDword(DevExt, Offset);
    DevExt->VmdChildCfg = save;
    DevExt->BusNumber = bus;
    DevExt->SlotNumber = slot;
    return v;
}

static VOID
NvmeVmdHostWrite(IN PVOID Context, IN ULONG Offset, IN ULONG Value)
{
    PHW_DEVICE_EXTENSION DevExt = (PHW_DEVICE_EXTENSION)Context;
    PUCHAR save = DevExt->VmdChildCfg;
    ULONG bus = DevExt->BusNumber, slot = DevExt->SlotNumber;
    DevExt->VmdChildCfg = NULL;
    DevExt->BusNumber = DevExt->VmdBus;
    DevExt->SlotNumber = DevExt->VmdSlot;
    WritePciConfigDword(DevExt, Offset, Value);
    DevExt->VmdChildCfg = save;
    DevExt->BusNumber = bus;
    DevExt->SlotNumber = slot;
}

/* NVMe controllers (01:08:02) */
static BOOLEAN
NvmeVmdMatch(IN PVOID Context, IN PUCHAR Cfg)
{
    (VOID)Context;
    return (BOOLEAN)((VmdCfgRead(Cfg, 8) >> 8) == 0x010802);
}

/*
 * Take NVMe controller number Child behind the VMD at Bus/Slot: the domain
 * set up, the controller's configuration space mapped (DevExt->VmdChildCfg:
 * ReadPciConfig* and WritePciConfig* go there from now on), its BAR0 put in
 * ConfigInfo's first access range, no interrupt (Fast Polling). FALSE: no
 * such controller.
 */
BOOLEAN
NvmeVmdAttach(IN PHW_DEVICE_EXTENSION DevExt, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
              IN ULONG Bus, IN ULONG Slot, IN PUCHAR VmdConfig, IN ULONG Child)
{
    PVMD_DOMAIN d = &DevExt->Vmd;
    ULONG childBus = 0, childDevFn = 0, count = 0, length = 0;
    BOOLEAN assigned = FALSE;
    PACCESS_RANGE range;
    PUCHAR cfg;
    ULONGLONG bar0;

    DevExt->VmdChildCfg = NULL;
    DevExt->VmdBus = Bus;
    DevExt->VmdSlot = Slot;
    DevExt->VmdChild = Child;
    DevExt->VmdMapType = ConfigInfo->AdapterInterfaceType;
    DevExt->VmdMapBus = ConfigInfo->SystemIoBusNumber;
    d->Context = DevExt;
    d->Map = NvmeVmdMap;
    d->HostRead = NvmeVmdHostRead;
    d->HostWrite = NvmeVmdHostWrite;
    d->Match = NvmeVmdMatch;
    if (!VmdOpen(d, *(PUSHORT)&VmdConfig[PCI_VENDOR_ID_OFFSET], *(PUSHORT)&VmdConfig[PCI_DEVICE_ID_OFFSET])) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: VMD %04X: BARs not assigned or not mapped\n",
                       *(PUSHORT)&VmdConfig[PCI_DEVICE_ID_OFFSET]);
#endif
        return FALSE;
    }
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: VMD %04X at bus %u slot %u: CFGBAR %08X%08X (%u buses from %u), MEMBAR1 %08X%08X+%08X\n",
                   *(PUSHORT)&VmdConfig[PCI_DEVICE_ID_OFFSET], Bus, Slot,
                   (ULONG)(d->CfgPhys >> 32), (ULONG)d->CfgPhys, d->BusCount, d->BusStart,
                   (ULONG)(d->Mem1Phys >> 32), (ULONG)d->Mem1Phys, (ULONG)d->Mem1Length);
#endif
    if (!VmdFindFunction(d, Child, &childBus, &childDevFn, &count, &assigned)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: VMD: no NVMe #%u in the domain (%u found)\n", Child, count);
#endif
        return FALSE;
    }
    cfg = VmdCfgSpace(d, childBus, childDevFn);
    if (cfg == NULL) return FALSE;
    DevExt->VmdChildCfg = cfg;

    bar0 = VmdFunctionBar(d, cfg, 0, &length);
    DevExt->ControllerRegistersLength = length;
    range = &((*(ConfigInfo->AccessRanges))[0]);
    range->RangeStart.QuadPart = (LONGLONG)bar0;
    range->RangeLength = length;
    range->RangeInMemory = TRUE;

    /* no INTx in the domain: Fast Polling. Vista and later: the VMD's own
       message interrupt (MSISupported in the INF) stays in ConfigInfo, as
       ScsiPort there fails a PCI adapter it can connect no interrupt for;
       it never fires, as the controllers' MSI-X stays off. */
#if !defined(NVMENT_W9X) && (_WIN32_WINNT >= 0x500)
    if (!g_NvmeNt6)
#endif
    {
        ConfigInfo->BusInterruptLevel = 0;
        ConfigInfo->BusInterruptVector = 0;
    }

    DevExt->VendorId = (USHORT)VmdCfgRead(cfg, 0);
    DevExt->DeviceId = (USHORT)(VmdCfgRead(cfg, 0) >> 16);
    DevExt->RevisionId = (UCHAR)VmdCfgRead(cfg, 8);
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: VMD %s: NVMe #%u of %u at %02X:%02X.%u, %04X:%04X, BAR0 %08X%08X+%X\n",
                   assigned ? "domain assigned" : "set-up kept", Child, count, childBus, childDevFn >> 3,
                   childDevFn & 7, DevExt->VendorId, DevExt->DeviceId, (ULONG)(bar0 >> 32), (ULONG)bar0, length);
#endif
    return TRUE;
}

/* After a power transition: the firmware may have cleared the domain's bus
   numbers and windows; set them up again the same way */
BOOLEAN
NvmeVmdRestore(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG b = 0, f = 0, n = 0;
    BOOLEAN assigned;
    if (DevExt->VmdChildCfg == NULL) return TRUE;
    NvmeVmdHostWrite(DevExt, 4, (NvmeVmdHostRead(DevExt, 4) & 0xFFFF) | VMD_CMD_MEMORY | VMD_CMD_MASTER);
    if ((VmdCfgRead(DevExt->VmdChildCfg, 0) & 0xFFFF) != 0xFFFF &&
        (VmdCfgRead(DevExt->VmdChildCfg, 4) & VMD_CMD_MEMORY)) {
        return TRUE;
    }
    return VmdFindFunction(&DevExt->Vmd, DevExt->VmdChild, &b, &f, &n, &assigned);
}

/* ------------------------------------------------------------------ */
/* Intel RST NVMe remapping                                            */
/* ------------------------------------------------------------------ */

/* Before VMD (6th to 10th generation Core), Intel RST in RAID mode could
   hide up to three NVMe controllers inside the AHCI function: no
   configuration space of their own, their registers 16 KB each from
   16 KB into the AHCI BAR (512 KB then), their interrupt the AHCI
   function's (shared, no MSI-X). ABAR A4h (VSCAP) bit 0: remapping;
   800h: one bit per device; 880h + i * 80h: its class code. After Linux
   include/linux/ahci-remap.h. The controllers run in Fast Polling; their
   configuration space reads give the AHCI function's, writes are dropped. */
#define NVME_AHCI_VSCAP                 0xA4
#define NVME_AHCI_REMAP_CAP             0x800
#define NVME_AHCI_REMAP_DCC(i)          (0x880 + (i) * 0x80)
#define NVME_AHCI_REMAP_BASE(i)         (0x4000 + (i) * 0x4000)
#define NVME_AHCI_REMAP_SIZE            0x4000
#define NVME_AHCI_MAX_REMAP             3

/* An Intel AHCI or RAID function that may hide NVMe controllers */
BOOLEAN
NvmeIsRemapHost(IN PUCHAR PciConfig)
{
    return (BOOLEAN)(*(PUSHORT)&PciConfig[PCI_VENDOR_ID_OFFSET] == 0x8086 &&
                     PciConfig[PCI_CLASS_CODE_OFFSET + 2] == PCI_CLASS_MASS_STORAGE_CONTROLLER &&
                     (PciConfig[PCI_CLASS_CODE_OFFSET + 1] == 0x06 || PciConfig[PCI_CLASS_CODE_OFFSET + 1] == 0x04) &&
                     (*(PULONG)&PciConfig[0x24] & 0xFFFFFFF1) != 0 && (*(PULONG)&PciConfig[0x24] & 1) == 0);
}

/*
 * Take remapped NVMe controller number Child of the AHCI function at
 * Bus/Slot (PciConfig its header): its registers put in ConfigInfo's first
 * access range, no interrupt. FALSE: remapping off or no such controller.
 */
BOOLEAN
NvmeRemapAttach(IN PHW_DEVICE_EXTENSION DevExt, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                IN ULONG Bus, IN ULONG Slot, IN PUCHAR PciConfig, IN ULONG Child)
{
    ULONG abar = *(PULONG)&PciConfig[0x24] & 0xFFFFFFF0, cap, i, n = 0, size;
    PUCHAR regs;
    PACCESS_RANGE range;

    DevExt->VmdMapType = ConfigInfo->AdapterInterfaceType;
    DevExt->VmdMapBus = ConfigInfo->SystemIoBusNumber;
    DevExt->BusNumber = Bus;
    DevExt->SlotNumber = Slot;
    regs = (PUCHAR)NvmeVmdMap(DevExt, abar, 0x1000);
    if (regs == NULL) return FALSE;
    if (!(ScsiPortReadRegisterUlong((PULONG)(regs + NVME_AHCI_VSCAP)) & 1)) return FALSE;
    /* the BAR covers the remapped devices only at 512 KB */
    size = ReadPciConfigDword(DevExt, 0x24);
    WritePciConfigDword(DevExt, 0x24, 0xFFFFFFFF);
    size = ~(ReadPciConfigDword(DevExt, 0x24) & 0xFFFFFFF0) + 1;
    WritePciConfigDword(DevExt, 0x24, *(PULONG)&PciConfig[0x24]);
    if (size < 0x80000) return FALSE;
    cap = ScsiPortReadRegisterUlong((PULONG)(regs + NVME_AHCI_REMAP_CAP));
    for (i = 0; i < NVME_AHCI_MAX_REMAP; i++) {
        if (!(cap & (1UL << i))) continue;
        if (ScsiPortReadRegisterUlong((PULONG)(regs + NVME_AHCI_REMAP_DCC(i))) != 0x010802) continue;
        if (n++ == Child) break;
    }
    if (i == NVME_AHCI_MAX_REMAP) return FALSE;

    range = &((*(ConfigInfo->AccessRanges))[0]);
    range->RangeStart.QuadPart = (LONGLONG)(abar + NVME_AHCI_REMAP_BASE(i));
    range->RangeLength = NVME_AHCI_REMAP_SIZE;
    range->RangeInMemory = TRUE;
    DevExt->ControllerRegistersLength = NVME_AHCI_REMAP_SIZE;
    ConfigInfo->BusInterruptLevel = 0;
    ConfigInfo->BusInterruptVector = 0;
    DevExt->RemapNoConfig = TRUE;
    DevExt->VendorId = *(PUSHORT)&PciConfig[PCI_VENDOR_ID_OFFSET];
    DevExt->DeviceId = *(PUSHORT)&PciConfig[PCI_DEVICE_ID_OFFSET];
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: RST remapped NVMe #%u (device %u) in the AHCI BAR %08X at bus %u slot %u\n",
                   Child, i, abar, Bus, Slot);
#endif
    return TRUE;
}
