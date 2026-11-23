/*
 * NVMENT - IRP_MJ_DEVICE_CONTROL front end for Windows 2000/XP
 *
 * Built into the 2000/XP driver only.
 */

#include <ntddk.h>
#include <srb.h>
#include <ntddscsi.h>

#ifndef CM_RESOURCE_INTERRUPT_MESSAGE
#define CM_RESOURCE_INTERRUPT_MESSAGE 0x0002
#endif

#define NVME_PDO_POOL_TAG ((ULONG)'N' | ((ULONG)'V' << 8) | ((ULONG)'M' << 16) | ((ULONG)'E' << 24))

/* IOCTLs of the disk class driver that ScsiPort does not handle on a PDO */
#define NVME_IOCTL_IDE_PASS_THROUGH      0x0004D028
#define NVME_IOCTL_ATA_PASS_THROUGH      0x0004D02C

/* ATA_PASS_THROUGH_EX.AtaFlags */
#define NVME_ATA_FLAGS_DATA_IN           0x0002
#define NVME_ATA_FLAGS_DATA_OUT          0x0004
#define NVME_ATA_FLAGS_48BIT_COMMAND     0x0008
#define NVME_ATA_FLAGS_USE_DMA           0x0010

/* ATA_PASS_THROUGH_EX: DataBufferOffset, a ULONG_PTR (at 14h for a 32-bit
   caller, at 18h for a 64-bit one), the two task files after it */
#define NVME_APT_OFFSET_FIELD(ptr)       ((ptr) == 8 ? 0x18 : 0x14)
#define NVME_APT_HEADER(ptr)             (NVME_APT_OFFSET_FIELD(ptr) + (ptr) + 16)
/* IOCTL_IDE_PASS_THROUGH: IDEREGS, ULONG DataBufferSize, data */
#define NVME_IPT_HEADER                  12

#define NVME_PT_MAX_DATA                 0x20000
#define NVME_PT_SENSE                    32
#define NVME_SCSIOP_ATA_PASSTHROUGH16    0x85

static PDRIVER_DISPATCH g_NvmeOrigDeviceControl;

/* No CRT */
static VOID
NvmePdoZero(OUT PVOID Dst, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    while (Bytes--) *d++ = 0;
}

static VOID
NvmePdoCopy(OUT PVOID Dst, IN CONST VOID *Src, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    CONST UCHAR *s = (CONST UCHAR *)Src;
    while (Bytes--) *d++ = *s++;
}

static NTSTATUS NTAPI
NvmePdoIoctlDone(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN PVOID Context)
{
    (VOID)Device;
    (VOID)Irp;
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* One SRB through the PDO, which puts the disk's address into it; the data
   (nonpaged pool) is mapped through an MDL, as ScsiPort maps the data of
   every request with DATA_IN or DATA_OUT through the IRP's MDL */
static NTSTATUS
NvmePdoSendSrb(IN PDEVICE_OBJECT Device, IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    PIO_STACK_LOCATION next;
    KEVENT event;
    NTSTATUS status;
    PMDL mdl = NULL;
    PIRP irp;

    irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (Length != 0) {
        mdl = IoAllocateMdl(Data, Length, FALSE, FALSE, irp);
        if (mdl == NULL) {
            IoFreeIrp(irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mdl);
    }
    Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
    Srb->DataBuffer = Data;
    Srb->DataTransferLength = Length;
    Srb->OriginalRequest = irp;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_SCSI;
    next->Parameters.Scsi.Srb = Srb;
    IoSetCompletionRoutine(irp, NvmePdoIoctlDone, &event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Device, irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    irp->MdlAddress = NULL;
    if (mdl != NULL) IoFreeMdl(mdl);
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS
NvmePdoComplete(IN PIRP Irp, IN NTSTATUS Status, IN ULONG_PTR Information)
{
    Irp->IoStatus.Information = Information;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* ------------------------------------------------------------------ */
/* IOCTL_SCSI_MINIPORT                                                 */
/* ------------------------------------------------------------------ */

/* disk.sys sends its SMART requests (SMART_GET_VERSION, SMART_RCV_DRIVE_DATA,
   SMART_SEND_DRIVE_COMMAND and its own failure prediction) as
   IOCTL_SCSI_MINIPORT to the disk's PDO. ScsiPort of Server 2003 / XP x64
   answers that IOCTL on a PDO with STATUS_INVALID_DEVICE_REQUEST (only the
   adapter's device object takes it). The front end turns it into the
   SRB_FUNCTION_IO_CONTROL request ScsiPort builds for the adapter, sent
   through the PDO, which puts the disk's address into it. Same on every
   2000/XP version: the miniport sees the request as before. */
static NTSTATUS
NvmePdoMiniportIoctl(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PSRB_IO_CONTROL ctl = (PSRB_IO_CONTROL)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PSCSI_REQUEST_BLOCK srb;
    ULONG length;
    NTSTATUS status;

    if (ctl == NULL || inLen < sizeof(SRB_IO_CONTROL)) {
        return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (ctl->HeaderLength != sizeof(SRB_IO_CONTROL)) {
        return NvmePdoComplete(Irp, STATUS_REVISION_MISMATCH, 0);
    }
    length = ctl->HeaderLength + ctl->Length;
    if (length < ctl->Length) {
        return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (inLen < length && outLen < length) {
        return NvmePdoComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK), NVME_PDO_POOL_TAG);
    if (srb == NULL) return NvmePdoComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    NvmePdoZero(srb, sizeof(*srb));
    srb->Function = SRB_FUNCTION_IO_CONTROL;
    srb->SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb->TimeOutValue = ctl->Timeout;
    status = NvmePdoSendSrb(Device, srb, ctl, length);
    ExFreePool(srb);
    return NvmePdoComplete(Irp, status, length < outLen ? length : outLen);
}

/* ------------------------------------------------------------------ */
/* IOCTL_ATA_PASS_THROUGH and IOCTL_IDE_PASS_THROUGH                    */
/* ------------------------------------------------------------------ */

/* The disk class driver passes both on to the PDO unchanged; ScsiPort
   knows neither (Windows XP sends IOCTL_ATA_PASS_THROUGH to an ATA port
   driver only). The front end sends the command as SCSI ATA PASS-THROUGH
   (16) with CK_COND, which the miniport carries out as its SAT layer
   does, and returns the ATA registers of the result from the sense data
   (ATA Status Return descriptor, or the fixed format of SAT). An ATA error
   completes the IOCTL with success and ERR in the status register, as the
   ATA port driver does. IOCTL_ATA_PASS_THROUGH_DIRECT (a buffer of the
   caller, not copied) stays unsupported. */

/* Commands that write data (IOCTL_IDE_PASS_THROUGH has no direction) */
static BOOLEAN
NvmePdoIdeWrites(IN CONST UCHAR *Regs)
{
    static CONST UCHAR writes[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x39, 0x3A, 0x3B, 0x3D, 0x3E, 0x3F, 0x57,
        0x92, 0x93, 0xC5, 0xCA, 0xCB, 0xCC, 0xCE, 0xE8, 0xF1, 0xF2, 0xF4, 0xF6
    };
    ULONG i;

    if (Regs[6] == 0xB0) return (BOOLEAN)(Regs[0] == 0xD6);      /* SMART WRITE LOG */
    for (i = 0; i < sizeof(writes); i++) if (writes[i] == Regs[6]) return TRUE;
    return FALSE;
}

/* DMA and 48-bit commands among those IOCTL_IDE_PASS_THROUGH may carry */
static BOOLEAN
NvmePdoIdeDma(IN UCHAR Command)
{
    return (BOOLEAN)(Command == 0xC8 || Command == 0xCA || Command == 0x25 || Command == 0x35 ||
                     Command == 0x3D || Command == 0x47 || Command == 0x57 || Command == 0x92 ||
                     Command == 0x93 || Command == 0x06 || Command == 0x07);
}

static BOOLEAN
NvmePdoIde48(IN UCHAR Command)
{
    return (BOOLEAN)(Command == 0x24 || Command == 0x25 || Command == 0x27 || Command == 0x29 ||
                     Command == 0x2F || Command == 0x34 || Command == 0x35 || Command == 0x37 ||
                     Command == 0x39 || Command == 0x3D || Command == 0x3F || Command == 0x42 ||
                     Command == 0x47 || Command == 0x57);
}

/* The registers of the result from the sense data: Cur/Prev in task file
   order (features/error, count, LBA low, mid, high, device, command/status) */
static BOOLEAN
NvmePdoSenseRegs(IN CONST UCHAR *Sense, IN ULONG Length, OUT PUCHAR Cur, OUT PUCHAR Prev)
{
    ULONG pos, end;

    if (Length < 8) return FALSE;
    if ((Sense[0] & 0x7F) == 0x72 || (Sense[0] & 0x7F) == 0x73) {
        end = 8 + (ULONG)Sense[7];
        if (end > Length) end = Length;
        for (pos = 8; pos + 2 <= end; pos += 2 + (ULONG)Sense[pos + 1]) {
            if (Sense[pos] == 0x09 && Sense[pos + 1] >= 0x0C && pos + 14 <= end) {
                Cur[0] = Sense[pos + 3];
                Prev[1] = Sense[pos + 4];
                Cur[1] = Sense[pos + 5];
                Prev[2] = Sense[pos + 6];
                Cur[2] = Sense[pos + 7];
                Prev[3] = Sense[pos + 8];
                Cur[3] = Sense[pos + 9];
                Prev[4] = Sense[pos + 10];
                Cur[4] = Sense[pos + 11];
                Cur[5] = Sense[pos + 12];
                Cur[6] = Sense[pos + 13];
                return TRUE;
            }
        }
        return FALSE;
    }
    if ((Sense[0] & 0x7F) == 0x70 || (Sense[0] & 0x7F) == 0x71) {
        /* SAT fixed format: INFORMATION = error, status, device, count;
           COMMAND-SPECIFIC INFORMATION = flags, LBA low, mid, high */
        if (Length < 12 || Sense[12] != 0x00 || Sense[13] != 0x1D) return FALSE;
        Cur[0] = Sense[3];
        Cur[6] = Sense[4];
        Cur[5] = Sense[5];
        Cur[1] = Sense[6];
        Cur[2] = Sense[9];
        Cur[3] = Sense[10];
        Cur[4] = Sense[11];
        return TRUE;
    }
    return FALSE;
}

static NTSTATUS
NvmePdoAtaPassThrough(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN BOOLEAN Ide)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR buf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG bufLen = inLen > outLen ? inLen : outLen;
    ULONG header, size, offset, timeout, ptr = sizeof(PVOID), done;
    USHORT flags;
    UCHAR cur[8], prev[8], res[8], resPrev[8];
    UCHAR protocol;
    BOOLEAN dataIn, dataOut, extend;
    PSCSI_REQUEST_BLOCK srb;
    PUCHAR sense, data = NULL;
    NTSTATUS status;
    UCHAR srbStatus;

    if (buf == NULL) return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
#if defined(_WIN64)
    if (!Ide && IoIs32bitProcess(Irp)) ptr = 4;
#endif
    NvmePdoZero(prev, sizeof(prev));
    if (Ide) {
        header = NVME_IPT_HEADER;
        if (inLen < header) return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        NvmePdoCopy(cur, buf, 8);
        size = *(ULONG UNALIGNED *)(buf + 8);
        offset = header;
        timeout = 10;
        dataOut = (BOOLEAN)(size != 0 && NvmePdoIdeWrites(cur));
        dataIn = (BOOLEAN)(size != 0 && !dataOut);
        extend = NvmePdoIde48(cur[6]);
        flags = (USHORT)(NvmePdoIdeDma(cur[6]) ? NVME_ATA_FLAGS_USE_DMA : 0);
        if (cur[6] == 0xB0) {                   /* SMART: the signature, as SMART_SEND_DRIVE_COMMAND */
            cur[3] = 0x4F;
            cur[4] = 0xC2;
        }
    } else {
        header = NVME_APT_HEADER(ptr);
        if (inLen < header || *(USHORT UNALIGNED *)buf != header) {
            return NvmePdoComplete(Irp, STATUS_REVISION_MISMATCH, 0);
        }
        flags = *(USHORT UNALIGNED *)(buf + 2);
        size = *(ULONG UNALIGNED *)(buf + 8);
        timeout = *(ULONG UNALIGNED *)(buf + 12);
        offset = (ptr == 8) ? (ULONG)*(ULONGLONG UNALIGNED *)(buf + NVME_APT_OFFSET_FIELD(ptr))
                            : *(ULONG UNALIGNED *)(buf + NVME_APT_OFFSET_FIELD(ptr));
        if (ptr == 8 && *(ULONGLONG UNALIGNED *)(buf + NVME_APT_OFFSET_FIELD(ptr)) > 0xFFFFFFFF) {
            return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        NvmePdoCopy(prev, buf + NVME_APT_OFFSET_FIELD(ptr) + ptr, 8);
        NvmePdoCopy(cur, buf + NVME_APT_OFFSET_FIELD(ptr) + ptr + 8, 8);
        dataIn = (BOOLEAN)((flags & NVME_ATA_FLAGS_DATA_IN) != 0);
        dataOut = (BOOLEAN)((flags & NVME_ATA_FLAGS_DATA_OUT) != 0);
        extend = (BOOLEAN)((flags & NVME_ATA_FLAGS_48BIT_COMMAND) != 0);
        if (dataIn && dataOut) return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        if (!dataIn && !dataOut) size = 0;
    }
    if (size > NVME_PT_MAX_DATA) return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    if (size != 0) {
        if (offset < header || offset > bufLen || size > bufLen - offset ||
            (dataIn && offset + size > outLen) || (dataOut && offset + size > inLen)) {
            return NvmePdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else {
        dataIn = dataOut = FALSE;
    }
    if (timeout == 0) timeout = 10;

    /* SRB, sense buffer and a copy of the data in one nonpaged block */
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool,
              sizeof(SCSI_REQUEST_BLOCK) + NVME_PT_SENSE + size, NVME_PDO_POOL_TAG);
    if (srb == NULL) return NvmePdoComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    NvmePdoZero(srb, sizeof(SCSI_REQUEST_BLOCK) + NVME_PT_SENSE);
    sense = (PUCHAR)(srb + 1);
    if (size != 0) {
        data = sense + NVME_PT_SENSE;
        if (dataOut) NvmePdoCopy(data, buf + offset, size); else NvmePdoZero(data, size);
    }

    if (!dataIn && !dataOut) protocol = 3;                         /* non-data */
    else if (flags & NVME_ATA_FLAGS_USE_DMA) protocol = 6;          /* DMA */
    else protocol = (UCHAR)(dataIn ? 4 : 5);                        /* PIO in / out */
    srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
    srb->CdbLength = 16;
    srb->SrbFlags = SRB_FLAGS_NO_QUEUE_FREEZE | (dataIn ? SRB_FLAGS_DATA_IN : 0) | (dataOut ? SRB_FLAGS_DATA_OUT : 0);
    if (!dataIn && !dataOut) srb->SrbFlags |= SRB_FLAGS_NO_DATA_TRANSFER;
    srb->TimeOutValue = timeout;
    srb->SenseInfoBuffer = sense;
    srb->SenseInfoBufferLength = NVME_PT_SENSE;
    srb->Cdb[0] = NVME_SCSIOP_ATA_PASSTHROUGH16;
    srb->Cdb[1] = (UCHAR)((protocol << 1) | (extend ? 1 : 0));
    /* CK_COND; T_DIR, BYT_BLOK, T_LENGTH in the sector count */
    srb->Cdb[2] = (UCHAR)(0x20 | (size != 0 ? (0x04 | 0x02 | (dataIn ? 0x08 : 0)) : 0));
    srb->Cdb[3] = extend ? prev[0] : 0;
    srb->Cdb[4] = cur[0];
    srb->Cdb[5] = extend ? prev[1] : 0;
    srb->Cdb[6] = cur[1];
    srb->Cdb[7] = extend ? prev[2] : 0;
    srb->Cdb[8] = cur[2];
    srb->Cdb[9] = extend ? prev[3] : 0;
    srb->Cdb[10] = cur[3];
    srb->Cdb[11] = extend ? prev[4] : 0;
    srb->Cdb[12] = cur[4];
    srb->Cdb[13] = cur[5];
    srb->Cdb[14] = cur[6];

    status = NvmePdoSendSrb(Device, srb, data, size);
    srbStatus = SRB_STATUS(srb->SrbStatus);
    NvmePdoZero(res, sizeof(res));
    NvmePdoZero(resPrev, sizeof(resPrev));
    if ((srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) &&
        NvmePdoSenseRegs(sense, NVME_PT_SENSE, res, resPrev)) {
        status = STATUS_SUCCESS;
    } else if (srbStatus == SRB_STATUS_SUCCESS || srbStatus == SRB_STATUS_DATA_OVERRUN) {
        res[6] = 0x50;                          /* DRDY DSC: done, no registers returned */
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        status = STATUS_IO_DEVICE_ERROR;
    }
    if (!NT_SUCCESS(status)) {
        ExFreePool(srb);
        return NvmePdoComplete(Irp, status, 0);
    }

    done = (dataIn && srb->DataTransferLength < size) ? srb->DataTransferLength : size;
    if (dataIn) NvmePdoCopy(buf + offset, data, done);
    ExFreePool(srb);
    res[7] = 0;
    if (Ide) {
        NvmePdoCopy(buf, res, 8);
        return NvmePdoComplete(Irp, STATUS_SUCCESS, dataIn ? header + done : header);
    }
    *(ULONG UNALIGNED *)(buf + 8) = done;
    if (extend) NvmePdoCopy(buf + NVME_APT_OFFSET_FIELD(ptr) + ptr, resPrev, 8);
    NvmePdoCopy(buf + NVME_APT_OFFSET_FIELD(ptr) + ptr + 8, res, 8);
    return NvmePdoComplete(Irp, STATUS_SUCCESS, dataIn ? offset + done : (outLen < header ? outLen : header));
}

/* ScsiPort creates the adapter's device object as FILE_DEVICE_CONTROLLER,
   its PDOs with the type of the device (disk, CD-ROM, mass storage) */
static NTSTATUS NTAPI
NvmeDeviceControlDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (Device->DeviceType != FILE_DEVICE_CONTROLLER && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (code == IOCTL_SCSI_MINIPORT) return NvmePdoMiniportIoctl(Device, Irp);
        if (code == NVME_IOCTL_ATA_PASS_THROUGH) return NvmePdoAtaPassThrough(Device, Irp, FALSE);
        if (code == NVME_IOCTL_IDE_PASS_THROUGH) return NvmePdoAtaPassThrough(Device, Irp, TRUE);
    }
    return g_NvmeOrigDeviceControl(Device, Irp);
}

/* After ScsiPortInitialize: ScsiPort's IRP_MJ_DEVICE_CONTROL entry gets a
   front end */
VOID
NvmeHookDeviceControl(IN PVOID DriverObject)
{
    PDRIVER_OBJECT driver = (PDRIVER_OBJECT)DriverObject;

    if (driver == NULL || driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] == NULL ||
        driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] == NvmeDeviceControlDispatch) {
        return;
    }
    g_NvmeOrigDeviceControl = driver->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NvmeDeviceControlDispatch;
}

/* ------------------------------------------------------------------ */
/* Start of an adapter without an interrupt (Intel VMD)                */
/* ------------------------------------------------------------------ */

/* Exported by every HAL; not in every DDK header for every platform */
ULONG NTAPI HalGetInterruptVector(INTERFACE_TYPE InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel,
                                  ULONG BusInterruptVector, PKIRQL Irql, PKAFFINITY Affinity);

/* An Intel VMD has no INTx pin (it signals MSI-X only, which ScsiPort does
   not use), and ScsiPort does not start a PCI adapter whose resources hold
   no interrupt. The start IRP of such an adapter gets one that never fires:
   ISA IRQ 13 (the 387's error line, unused since the 486), edge-triggered
   and shared. The controllers behind the VMD run in Fast Polling, and
   HwInterrupt finds nothing to do should IRQ 13 come anyway. */
#define NVME_NOIRQ_IRQ                   13

/* Vista and later (NvmeHookPnp). There the start of a VMD carries the VMD's
   own message interrupts (its MSI-X table), which ScsiPort cannot connect
   (STATUS_INVALID_PARAMETER): they are left out. The HAL gives no vector for
   a line not in use there (IRQ 13 included), so the VMD shares a
   level-triggered line in use instead: the ACPI SCI, else a PCI line; the
   interrupt routine finds nothing to do on it. A line asked for in the
   resource requirements is not granted to a device without an INTx pin
   (problem 12). */
BOOLEAN g_NvmeNt6;
#define NVME_PNP_LIST_SLOTS              8

typedef struct _NVME_PNP_LISTS {
    PDEVICE_OBJECT Device;
    PCM_RESOURCE_LIST Raw;
    PCM_RESOURCE_LIST Translated;
} NVME_PNP_LISTS;

static NVME_PNP_LISTS g_NvmePnpLists[NVME_PNP_LIST_SLOTS];
static PDRIVER_DISPATCH g_NvmeOrigPnp;

static PCM_PARTIAL_RESOURCE_DESCRIPTOR
NvmeDescriptors(IN PCM_RESOURCE_LIST List)
{
    return (PCM_PARTIAL_RESOURCE_DESCRIPTOR)((PUCHAR)List +
               FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors));
}

/* Size of a one-list resource list, 0 for another kind; *Interrupts: its interrupts */
static ULONG
NvmeResourceListSize(IN PCM_RESOURCE_LIST List, OUT PULONG Count, OUT PULONG Interrupts)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    ULONG i, size;

    *Count = 0;
    *Interrupts = 0;
    if (List == NULL || List->Count != 1) return 0;
    *Count = List->List[0].PartialResourceList.Count;
    size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors);
    d = NvmeDescriptors(List);
    for (i = 0; i < *Count; i++) {
        size += sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
        if (d[i].Type == CmResourceTypeInterrupt && !(d[i].Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) (*Interrupts)++;
        if (d[i].Type == CmResourceTypeDeviceSpecific) {
            size += d[i].u.DeviceSpecificData.DataSize;
            *Count = i + 1;
            break;
        }
    }
    return size;
}

/* A copy of the list with the interrupt added (in front of device-specific data) */
static PCM_RESOURCE_LIST
NvmeAddIrq(IN PCM_RESOURCE_LIST List, IN BOOLEAN Translated, IN ULONG Irq)
{
    PCM_RESOURCE_LIST copy;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR src, d;
    ULONG size, count, ints, i, n = 0, tail = 0;

    size = NvmeResourceListSize(List, &count, &ints);
    if (size == 0) return NULL;
    copy = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(NonPagedPool, size + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR),
                                                    NVME_PDO_POOL_TAG);
    if (copy == NULL) return NULL;
    NvmePdoCopy(copy, List, FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors));
    src = NvmeDescriptors(List);
    d = NvmeDescriptors(copy);
    for (i = 0; i < count; i++) {
        if (src[i].Type == CmResourceTypeDeviceSpecific) {
            tail = i + 1;
            break;
        }
        /* message interrupts (a VMD's own MSI-X, Vista and later): ScsiPort
           cannot connect them */
        if (src[i].Type == CmResourceTypeInterrupt && (src[i].Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) continue;
        d[n++] = src[i];
    }
    NvmePdoZero(&d[n], sizeof(d[n]));
    d[n].Type = CmResourceTypeInterrupt;
    d[n].ShareDisposition = CmResourceShareShared;
    /* IRQ 13 edge-triggered; Vista and later: a level-triggered line shared
       with its owner */
    d[n].Flags = g_NvmeNt6 ? CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE : CM_RESOURCE_INTERRUPT_LATCHED;
    if (Translated) {
        KIRQL irql = 0;
        KAFFINITY affinity = 0;
        d[n].u.Interrupt.Vector = HalGetInterruptVector(g_NvmeNt6 ? PCIBus : Isa, 0, Irq, Irq, &irql, &affinity);
        d[n].u.Interrupt.Level = irql;
        d[n].u.Interrupt.Affinity = affinity;
        if (d[n].u.Interrupt.Vector == 0) {
            ExFreePool(copy);
            return NULL;
        }
    } else {
        d[n].u.Interrupt.Level = Irq;
        d[n].u.Interrupt.Vector = Irq;
        d[n].u.Interrupt.Affinity = (KAFFINITY)-1;
    }
    n++;
    if (tail != 0) {
        /* the device-specific descriptor and its data, last */
        NvmePdoCopy(&d[n], &src[tail - 1], (ULONG)((PUCHAR)List + size - (PUCHAR)&src[tail - 1]));
        n++;
    }
    copy->List[0].PartialResourceList.Count = n;
    return copy;
}

/* The dormant interrupt: IRQ 13; Vista and later, where the HAL gives a
   vector only for a line in use: the ACPI SCI (9), else a PCI line (16-23),
   both level-triggered and shareable. 0: none. */
static ULONG
NvmeDormantIrq(VOID)
{
    static const UCHAR lines[] = { 9, 16, 17, 18, 19, 20, 21, 22, 23 };
    ULONG i;

    if (!g_NvmeNt6) return NVME_NOIRQ_IRQ;
    for (i = 0; i < sizeof(lines); i++) {
        KIRQL irql = 0;
        KAFFINITY affinity = 0;
        if (HalGetInterruptVector(PCIBus, 0, lines[i], lines[i], &irql, &affinity) != 0 && affinity != 0) {
            return lines[i];
        }
    }
    return 0;
}

static VOID
NvmeFreePnpLists(IN PDEVICE_OBJECT Device)
{
    ULONG i;
    for (i = 0; i < NVME_PNP_LIST_SLOTS; i++) {
        if (g_NvmePnpLists[i].Device == Device) {
            if (g_NvmePnpLists[i].Raw != NULL) ExFreePool(g_NvmePnpLists[i].Raw);
            if (g_NvmePnpLists[i].Translated != NULL) ExFreePool(g_NvmePnpLists[i].Translated);
            NvmePdoZero(&g_NvmePnpLists[i], sizeof(g_NvmePnpLists[i]));
        }
    }
}



static NTSTATUS NTAPI
NvmePnpDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = stack->MinorFunction;
    NTSTATUS status;

    if (minor == IRP_MN_START_DEVICE && Device->DeviceType == FILE_DEVICE_CONTROLLER) {
        PCM_RESOURCE_LIST raw = stack->Parameters.StartDevice.AllocatedResources;
        PCM_RESOURCE_LIST translated = stack->Parameters.StartDevice.AllocatedResourcesTranslated;
        ULONG count, rawInts = 0, trInts = 0, i;
#ifdef NVMENT_DBG
        {
            ULONG c2 = 0, n2 = 0, k;
            if (translated != NULL && NvmeResourceListSize(translated, &c2, &n2) != 0) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR dd = NvmeDescriptors(translated);
                for (k = 0; k < c2; k++) {
                    ScsiDebugPrint(0, "NVMENT: start resource %u: type %u flags %04X share %u level %u vector %u\n", k,
                                   dd[k].Type, dd[k].Flags, dd[k].ShareDisposition, dd[k].u.Interrupt.Level,
                                   dd[k].u.Interrupt.Vector);
                }
            }
        }
#endif

        if (raw != NULL && translated != NULL &&
            NvmeResourceListSize(raw, &count, &rawInts) != 0 &&
            NvmeResourceListSize(translated, &count, &trInts) != 0 &&
            rawInts == 0 && trInts == 0) {
            ULONG irq = NvmeDormantIrq();
            PCM_RESOURCE_LIST newRaw = irq != 0 ? NvmeAddIrq(raw, FALSE, irq) : NULL;
            PCM_RESOURCE_LIST newTranslated = newRaw != NULL ? NvmeAddIrq(translated, TRUE, irq) : NULL;
            if (newRaw != NULL && newTranslated != NULL) {
                NvmeFreePnpLists(Device);
                for (i = 0; i < NVME_PNP_LIST_SLOTS; i++) {
                    if (g_NvmePnpLists[i].Device == NULL) {
                        g_NvmePnpLists[i].Device = Device;
                        g_NvmePnpLists[i].Raw = newRaw;
                        g_NvmePnpLists[i].Translated = newTranslated;
                        break;
                    }
                }
                stack->Parameters.StartDevice.AllocatedResources = newRaw;
                stack->Parameters.StartDevice.AllocatedResourcesTranslated = newTranslated;
            } else {
                if (newRaw != NULL) ExFreePool(newRaw);
                if (newTranslated != NULL) ExFreePool(newTranslated);
            }
        }
    }
    status = g_NvmeOrigPnp(Device, Irp);
#ifdef NVMENT_DBG
    if (minor == IRP_MN_START_DEVICE) {
        ScsiDebugPrint(0, "NVMENT: start status %08X\n", status);
    }
#endif
    if (minor == IRP_MN_REMOVE_DEVICE) NvmeFreePnpLists(Device);
    return status;
}

/* After ScsiPortInitialize: ScsiPort's IRP_MJ_PNP entry gets a front end */
VOID
NvmeHookPnp(IN PVOID DriverObject)
{
    PDRIVER_OBJECT driver = (PDRIVER_OBJECT)DriverObject;

    if (driver == NULL || driver->MajorFunction[IRP_MJ_PNP] == NULL ||
        driver->MajorFunction[IRP_MJ_PNP] == NvmePnpDispatch) {
        return;
    }
    {
        ULONG major = 0, minor = 0;
        PsGetVersion(&major, &minor, NULL, NULL);
        g_NvmeNt6 = (BOOLEAN)(major >= 6);
    }
    g_NvmeOrigPnp = driver->MajorFunction[IRP_MJ_PNP];
    driver->MajorFunction[IRP_MJ_PNP] = NvmePnpDispatch;
}
