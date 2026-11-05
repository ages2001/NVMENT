// building and sending NVME commands
#include "nvment.h"
#include "nvmeutil.h"

//
// NVMe Register Access Functions
//

ULONG NvmeReadReg32(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset)
{
    return ScsiPortReadRegisterUlong((PULONG)((PUCHAR)DevExt->ControllerRegisters + Offset));
}

VOID NvmeWriteReg32(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN ULONG Value)
{
    ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)DevExt->ControllerRegisters + Offset), Value);
}

ULONGLONG NvmeReadReg64(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset)
{
    ULONGLONG value;
    PULONG ptr = (PULONG)((PUCHAR)DevExt->ControllerRegisters + Offset);
    
    return (ULONGLONG)ScsiPortReadRegisterUlong(ptr)
         | (((ULONGLONG)ScsiPortReadRegisterUlong(ptr + 1)) << 32);
}

VOID NvmeWriteReg64(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN ULONGLONG Value)
{
    PULONG ptr = (PULONG)((PUCHAR)DevExt->ControllerRegisters + Offset);
    
    ScsiPortWriteRegisterUlong(ptr, (ULONG)(Value & 0xFFFFFFFF));
    ScsiPortWriteRegisterUlong(ptr + 1, (ULONG)(Value >> 32));
}

//
// NvmeWaitForReady - Wait for controller ready status
//
BOOLEAN NvmeWaitForReady(IN PHW_DEVICE_EXTENSION DevExt, IN BOOLEAN WaitForReady)
{
    ULONG timeout = 5000;  // 5 seconds
    ULONG csts;
    
    while (timeout > 0) {
        csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
        
        if (WaitForReady) {
            if (csts & NVME_CSTS_RDY) {
                return TRUE;
            }
        } else {
            if (!(csts & NVME_CSTS_RDY)) {
                return TRUE;
            }
        }
        
        ScsiPortStallExecution(1000);  // Wait 1ms
        timeout--;
    }
    
    return FALSE;
}

//
// NvmeRingDoorbell - Ring submission or completion queue doorbell
//
VOID NvmeRingDoorbell(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT QueueId, IN BOOLEAN IsSubmission, IN USHORT Value)
{
    ULONG offset;

    offset = NVME_REG_DBS + (2 * QueueId * DevExt->DoorbellStride);
    if (!IsSubmission) {
        offset += DevExt->DoorbellStride;
    }

    NvmeWriteReg32(DevExt, offset, Value);
}

//
// NvmeSubmitCommand - Submit a command to a queue
//
static BOOLEAN NvmeSubmitCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_QUEUE Queue, IN PNVME_COMMAND Cmd)
{
    PNVME_COMMAND sqEntry;
    USHORT nextTail;
    ULONG currentHead;
    BOOLEAN result;

    // Check if queue is full - SubmissionQueueHead is protected by lock
    nextTail = (USHORT)((Queue->SubmissionQueueTail + 1) & Queue->QueueSizeMask);
    currentHead = (USHORT)(Queue->SubmissionQueueHead & Queue->QueueSizeMask);
    if (nextTail == currentHead) {
        // Queue full - release lock and return
        return FALSE;
    }

    // Get submission queue entry
    sqEntry = (PNVME_COMMAND)((PUCHAR)Queue->SubmissionQueue +
                              (Queue->SubmissionQueueTail * NVME_SQ_ENTRY_SIZE));

    // Copy command to queue
    memcpy(sqEntry, Cmd, sizeof(NVME_COMMAND));

#ifdef NVMENT_DBG_CMD
    // Dump the command for debugging
    ScsiDebugPrint(0, "NVMENT: NvmeSubmitCommand - QID=%d Tail=%d\n", Queue->QueueId, Queue->SubmissionQueueTail);
    ScsiDebugPrint(0, "  CDW0=%08X (OPC=%02X Flags=%02X CID=%04X) NSID=%08X CDW2=%08X CDW3=%08X\n",
                   sqEntry->CDW0.AsUlong, sqEntry->CDW0.Fields.Opcode, sqEntry->CDW0.Fields.Flags,
                   sqEntry->CDW0.Fields.CommandId, sqEntry->NSID, sqEntry->CDW2, sqEntry->CDW3);
    ScsiDebugPrint(0, "  MPTR=%08X%08X\n",
                   (ULONG)(sqEntry->MPTR >> 32), (ULONG)(sqEntry->MPTR & 0xFFFFFFFF));
    ScsiDebugPrint(0, "  PRP1=%08X%08X PRP2=%08X%08X\n",
                   (ULONG)(sqEntry->PRP1 >> 32), (ULONG)(sqEntry->PRP1 & 0xFFFFFFFF),
                   (ULONG)(sqEntry->PRP2 >> 32), (ULONG)(sqEntry->PRP2 & 0xFFFFFFFF));
    ScsiDebugPrint(0, "  CDW10=%08X CDW11=%08X CDW12=%08X CDW13=%08X\n",
                   sqEntry->CDW10, sqEntry->CDW11, sqEntry->CDW12, sqEntry->CDW13);
    ScsiDebugPrint(0, "  CDW14=%08X CDW15=%08X\n",
                   sqEntry->CDW14, sqEntry->CDW15);
#endif
    // Update tail
    Queue->SubmissionQueueTail = nextTail;

    // Ring doorbell
    NvmeRingDoorbell(DevExt, Queue->QueueId, TRUE, (USHORT)(Queue->SubmissionQueueTail));

    return TRUE;
}

BOOLEAN NvmeSubmitIoCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd)
{
    BOOLEAN result;

    result = NvmeSubmitCommand(DevExt, &DevExt->IoQueue, Cmd);

    if (result) {
        NvmeArmWatchdog(DevExt);

        // Command successfully submitted - update queue depth tracking
        DevExt->CurrentQueueDepth++;

        // Update maximum queue depth if we've reached a new high
        if (DevExt->CurrentQueueDepth > DevExt->MaxQueueDepthReached) {
            DevExt->MaxQueueDepthReached = DevExt->CurrentQueueDepth;
        }
    }

    return result;
}

BOOLEAN NvmeSubmitAdminCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd)
{
    if (!NvmeSubmitCommand(DevExt, &DevExt->AdminQueue, Cmd)) {
        return FALSE;
    }
    if (DevExt->InitComplete && Cmd->CDW0.Fields.CommandId != ADMIN_CID_SYNC) {
        NvmeArmWatchdog(DevExt);
    }
    return TRUE;
}

//
// NvmeAdminSync - Submit an admin command and poll for its completion.
// Other admin completions found meanwhile are processed as usual. Used during
// (re)initialization and for the rare admin commands issued from HwStartIo,
// where ScsiPort already holds the interrupt spinlock.
//
BOOLEAN NvmeAdminSync(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd, IN ULONG TimeoutMs)
{
    ULONG elapsed;

    Cmd->CDW0.Fields.CommandId = ADMIN_CID_SYNC;
    DevExt->SyncDone = FALSE;
    DevExt->SyncStatus = 0;
    DevExt->SyncDw0 = 0;

    if (!NvmeSubmitCommand(DevExt, &DevExt->AdminQueue, Cmd)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeAdminSync - admin queue full (opc %02X)\n", Cmd->CDW0.Fields.Opcode);
#endif
        return FALSE;
    }

    for (elapsed = 0; elapsed < TimeoutMs * 100; elapsed++) {
        NvmeProcessAdminCompletion(DevExt);
        if (DevExt->SyncDone) {
#ifdef NVMENT_DBG
            if (DevExt->SyncStatus != NVME_SC_SUCCESS) {
                ScsiDebugPrint(0, "NVMENT: NvmeAdminSync - opc %02X status %04X\n",
                               Cmd->CDW0.Fields.Opcode, DevExt->SyncStatus);
            }
#endif
            return (BOOLEAN)(DevExt->SyncStatus == NVME_SC_SUCCESS);
        }
        if (NvmeReadReg32(DevExt, NVME_REG_CSTS) & NVME_CSTS_CFS) {
            break;
        }
        ScsiPortStallExecution(10);
    }
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeAdminSync - opc %02X timed out, CSTS=%08X\n",
                   Cmd->CDW0.Fields.Opcode, NvmeReadReg32(DevExt, NVME_REG_CSTS));
#endif
    return FALSE;
}

BOOLEAN NvmeCreateIoCQ(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;

    memset(&cmd, 0, sizeof(NVME_COMMAND));

    cmd.CDW0.Fields.Opcode = NVME_ADMIN_CREATE_CQ;
    cmd.CDW0.Fields.Flags = 0;
    cmd.CDW10 = ((DevExt->IoQueue.QueueSize - 1) << 16) | DevExt->IoQueue.QueueId;
    // CDW11: PC=1 (bit 0), IEN=1 (bit 1) to enable interrupts, IV=0 (bits 31:16)
    cmd.CDW11 = NVME_QUEUE_PHYS_CONTIG | NVME_QUEUE_IRQ_ENABLED | (0 << 16);
    cmd.PRP1 = DevExt->IoQueue.CompletionQueuePhys.QuadPart;

    return NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS);
}

BOOLEAN NvmeCreateIoSQ(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;

    memset(&cmd, 0, sizeof(NVME_COMMAND));

    cmd.CDW0.Fields.Opcode = NVME_ADMIN_CREATE_SQ;
    cmd.CDW0.Fields.Flags = 0;
    cmd.CDW10 = ((DevExt->IoQueue.QueueSize - 1) << 16) | DevExt->IoQueue.QueueId;
    cmd.CDW11 = NVME_QUEUE_PHYS_CONTIG | (DevExt->IoQueue.QueueId << 16);  // CQID
    cmd.PRP1 = DevExt->IoQueue.SubmissionQueuePhys.QuadPart;

    return NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS);
}

//
// NvmeIdentifySync - Identify (CNS, NSID) into the utility buffer
//
static BOOLEAN NvmeIdentifyInto(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId, IN ULONG CNS,
                                IN PVOID Buffer, IN PHYSICAL_ADDRESS BufferPhys, IN ULONG TimeoutMs)
{
    NVME_COMMAND cmd;

    memset(Buffer, 0, NVME_PAGE_SIZE);
    memset(&cmd, 0, sizeof(NVME_COMMAND));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_IDENTIFY;
    cmd.NSID = NamespaceId;
    cmd.PRP1 = BufferPhys.QuadPart;
    cmd.PRP2 = 0;  // Single page transfer, no PRP2 needed
    cmd.CDW10 = CNS;

    return NvmeAdminSync(DevExt, &cmd, TimeoutMs);
}

static BOOLEAN NvmeIdentifySync(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId, IN ULONG CNS)
{
    return NvmeIdentifyInto(DevExt, NamespaceId, CNS, DevExt->UtilityBuffer, DevExt->UtilityBufferPhys,
                            NVMENT_ADMIN_TIMEOUT_MS);
}

//
// NvmeIdentifyController - Identify Controller: strings, MDTS, features
//
BOOLEAN NvmeIdentifyController(IN PHW_DEVICE_EXTENSION DevExt)
{
    PNVME_IDENTIFY_CONTROLLER ctrlData = (PNVME_IDENTIFY_CONTROLLER)DevExt->UtilityBuffer;
    ULONG driverMaxTransfer;
    ULONG ps;

    if (!NvmeIdentifySync(DevExt, 0, NVME_CNS_CONTROLLER)) {
        return FALSE;
    }

    // Copy and null-terminate strings
    memcpy(DevExt->ControllerSerialNumber, ctrlData->SerialNumber, 20);
    DevExt->ControllerSerialNumber[20] = 0;

    memcpy(DevExt->ControllerModelNumber, ctrlData->ModelNumber, 40);
    DevExt->ControllerModelNumber[40] = 0;

    memcpy(DevExt->ControllerFirmwareRevision, ctrlData->FirmwareRevision, 8);
    DevExt->ControllerFirmwareRevision[8] = 0;

    DevExt->NumberOfNamespaces = ctrlData->NumberOfNamespaces;
    DevExt->Oncs = ctrlData->Oncs;
    DevExt->Oacs = ctrlData->Oacs;
    DevExt->Oaes = *(PULONG)((PUCHAR)ctrlData + 92);
    DevExt->Cntlid = *(PUSHORT)((PUCHAR)ctrlData + 78);
    DevExt->VwcPresent = (BOOLEAN)((ctrlData->Vwc & 1) != 0);
    DevExt->Npss = ctrlData->Npss;
    DevExt->Hmpre = *(PULONG)((PUCHAR)ctrlData + 272);
    DevExt->Hmmin = *(PULONG)((PUCHAR)ctrlData + 276);
    DevExt->Hmminds = *(PULONG)((PUCHAR)ctrlData + 332);
    DevExt->Hmmaxd = *(PUSHORT)((PUCHAR)ctrlData + 336);

    // Deepest non-operational power state, used for START STOP UNIT (stop)
    DevExt->IdlePowerState = 0;
    for (ps = 1; ps <= DevExt->Npss && ps < 32; ps++) {
        if (ctrlData->Psd[ps].Flags & NVME_PSD_NOPS) {
            DevExt->IdlePowerState = (UCHAR)ps;
        }
    }

    // APST table: every power state goes, after an idle time of 50 times
    // the round-trip latency, to the deepest non-operational state whose
    // exit latency stays within NVMENT_APST_MAX_LATENCY_US
    DevExt->ApstSupported = (BOOLEAN)((ctrlData->Reserved2[1] & 1) != 0 && DevExt->Npss != 0);
    memset(DevExt->ApstTable, 0, sizeof(DevExt->ApstTable));
    if (DevExt->ApstSupported) {
        ULONG target = 0, state, total, ms;
        for (state = DevExt->Npss + 1; state-- > 0; ) {
            if (state >= 32) continue;
            if (target != 0) DevExt->ApstTable[state * 2] = target;
            if (!(ctrlData->Psd[state].Flags & NVME_PSD_NOPS)) continue;
            if (ctrlData->Psd[state].ExitLatency > NVMENT_APST_MAX_LATENCY_US) continue;
            total = ctrlData->Psd[state].ExitLatency + ctrlData->Psd[state].EntryLatency;
            ms = (total + 19) / 20;
            if (ms > 0xFFFFFF) ms = 0xFFFFFF;
            target = (state << 3) | (ms << 8);
        }
    }

    // Read MDTS (Maximum Data Transfer Size)
    // Per NVMe spec: MDTS specifies the maximum data transfer size for a command
    // Value is in units of minimum memory page size (CAP.MPSMIN)
    // Maximum transfer = 2^MDTS * minimum page size
    // If MDTS is 0, there is no maximum transfer size limit from the controller
    DevExt->MaxDataTransferSizePower = ctrlData->MaxDataTransferSize;

    // Driver maximum: One PRP list page with 512 entries * 4KB per entry = 2MB
    driverMaxTransfer = 512 << NVME_PAGE_SHIFT;

    if (DevExt->MaxDataTransferSizePower == 0 || DevExt->MaxDataTransferSizePower > 9) {
        // No controller-imposed limit, or one above ours
        DevExt->MaxTransferSizeBytes = driverMaxTransfer;
    } else {
        // Calculate: 2^MDTS * PageSize (PageSize = 4KB)
        DevExt->MaxTransferSizeBytes = (1UL << (DevExt->MaxDataTransferSizePower + NVME_PAGE_SHIFT));

        // Clamp to driver maximum
        if (DevExt->MaxTransferSizeBytes > driverMaxTransfer) {
            DevExt->MaxTransferSizeBytes = driverMaxTransfer;
        }
    }
    // Set up again: never more than ScsiPort was told at the start
    if (DevExt->TransferReported && DevExt->MaxTransferSizeBytes > DevExt->ReportedTransfer) {
        DevExt->MaxTransferSizeBytes = DevExt->ReportedTransfer;
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Identified controller - Model: %.40s SN: %.20s FW: %.8s NN: %u\n",
                DevExt->ControllerModelNumber, DevExt->ControllerSerialNumber,
                DevExt->ControllerFirmwareRevision, DevExt->NumberOfNamespaces);
    ScsiDebugPrint(0, "NVMENT: MDTS=%u max transfer %u bytes, ONCS=%04X VWC=%u NPSS=%u idle PS=%u\n",
                DevExt->MaxDataTransferSizePower, DevExt->MaxTransferSizeBytes,
                DevExt->Oncs, DevExt->VwcPresent, DevExt->Npss, DevExt->IdlePowerState);
#endif
    return TRUE;
}

//
// NvmeAddNamespace - Identify one namespace and give it the next LUN
//
static BOOLEAN NvmeAddNamespaceData(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId,
                                    IN PNVME_IDENTIFY_NAMESPACE nsData, IN ULONG Slot);

static VOID NvmeAddNamespace(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId)
{
    if (DevExt->LunCount >= NVMENT_MAX_LUNS) {
        return;
    }
    if (!NvmeIdentifySync(DevExt, NamespaceId, NVME_CNS_NAMESPACE)) {
        return;
    }
    NvmeAddNamespaceData(DevExt, NamespaceId, (PNVME_IDENTIFY_NAMESPACE)DevExt->UtilityBuffer, DevExt->LunCount);
}

//
// NvmeAddNamespaceData - Give a namespace a LUN from its Identify data: the
// next one (Slot = LunCount), or the one it had before it went away
//
static BOOLEAN NvmeAddNamespaceData(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId,
                                    IN PNVME_IDENTIFY_NAMESPACE nsData, IN ULONG Slot)
{
    PNVME_LUN lun;
    UCHAR format;
    UCHAR lbads;
    USHORT ms, metaSize;
    BOOLEAN piAct;

    if (Slot >= NVMENT_MAX_LUNS || nsData->NamespaceSize == 0) {
        return FALSE;
    }

    // The LBA format in use: FLBAS bits 3:0 index LBAF0..15, LBADS is log2 of the block size
    format = (UCHAR)(nsData->FormattedLbaSize & NVME_FLBAS_INDEX_MASK);
    lbads = nsData->LbaFormats[format].LbaDataSize;
    ms = nsData->LbaFormats[format].MetadataSize;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NSID %u - blocks=%I64u LBAF%u (LBADS=%u MS=%u) NSFEAT=%02X\n",
                   NamespaceId, nsData->NamespaceSize, format, lbads, ms, nsData->NamespaceFeatures);
#endif

    // 512 to 4096-byte blocks. Metadata (MS != 0): protection information
    // alone (MS = 8 with DPS) is inserted and stripped by the controller
    // (PRACT); other metadata in a separate buffer (one PRP pool page per
    // command, MS up to NVMENT_MAX_META), protection information in it
    // generated and checked by the controller. Metadata inside the blocks
    // (extended LBA) cannot be shown as plain blocks.
    piAct = FALSE;
    metaSize = 0;
    if (ms != 0) {
        UCHAR dps = (UCHAR)(((PUCHAR)nsData)[29] & 0x07);
        if (nsData->FormattedLbaSize & NVME_FLBAS_EXTENDED) {
            ms = 0xFFFF;
        } else if (dps != 0 && ms == 8) {
            piAct = TRUE;
        } else if (ms <= NVMENT_MAX_META && lbads >= 9) {
            ULONG blocks = DevExt->MaxTransferSizeBytes >> lbads;
            metaSize = ms;
            piAct = (BOOLEAN)(dps != 0);
            if (blocks * ms > NVME_PAGE_SIZE) {
                if (!DevExt->TransferReported) {
                    // Before ScsiPort knows the limit: transfers small enough for one page
                    DevExt->MaxTransferSizeBytes = (NVME_PAGE_SIZE / ms) << lbads;
                } else {
                    ms = 0xFFFF;
                }
            }
        } else {
            ms = 0xFFFF;
        }
    }
    if (lbads < 9 || lbads > 12 || ms == 0xFFFF) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NSID %u - LBA format not supported, skipped\n", NamespaceId);
#endif
        return FALSE;
    }

    lun = &DevExt->Luns[Slot];
    lun->NamespaceId = NamespaceId;
    lun->GoneNsid = 0;
    lun->MetaSize = metaSize;
    lun->PiAct = piAct;
    lun->SizeInBlocks = nsData->NamespaceSize;
    lun->BlockShift = lbads;
    lun->BlockSize = 1UL << lbads;
    lun->LbaShift = 0;
    // 4K=1: a namespace of 2 TB and more with 512-byte blocks is shown with
    // 4096-byte blocks, so systems with 32-bit block addresses (READ(10),
    // an MBR) reach up to 16 TB of it
    if (DevExt->Sector4K && lbads == 9 && lun->SizeInBlocks >= 0x100000000ULL) {
        lun->LbaShift = 3;
        lun->BlockShift = 12;
        lun->BlockSize = 4096;
        lun->SizeInBlocks >>= 3;
    }
    lun->Thin = (BOOLEAN)((nsData->NamespaceFeatures & NVME_NSFEAT_THIN) != 0);
    lun->NonTaggedInFlight = NULL;
    if (Slot == DevExt->LunCount) {
        DevExt->LunCount++;
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: LUN %u = NSID %u, %u-byte blocks\n",
                   Slot, NamespaceId, lun->BlockSize);
#endif
    return TRUE;
}

//
// NvmeIdentifyNamespace - Find the active namespaces, one LUN each
//
BOOLEAN NvmeIdentifyNamespace(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG nsids[NVMENT_MAX_LUNS * 2];
    ULONG count = 0;
    ULONG i;

    DevExt->LunCount = 0;
    memset(DevExt->Luns, 0, sizeof(DevExt->Luns));

    // NVMe 1.1+: active namespace list; otherwise try the first namespace IDs
    if (DevExt->Version >= 0x00010100 && NvmeIdentifySync(DevExt, 0, NVME_CNS_ACTIVE_NS)) {
        PULONG list = (PULONG)DevExt->UtilityBuffer;
        while (count < NVMENT_MAX_LUNS * 2 && list[count] != 0) {
            nsids[count] = list[count];
            count++;
        }
    } else {
        for (i = 1; i <= DevExt->NumberOfNamespaces && count < NVMENT_MAX_LUNS * 2; i++) {
            nsids[count++] = i;
        }
    }

    for (i = 0; i < count && DevExt->LunCount < NVMENT_MAX_LUNS; i++) {
        ULONG k;
        for (k = 0; k < DevExt->NsOffCount && DevExt->NsOff[k] != nsids[i]; k++) ;
        if (k < DevExt->NsOffCount) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NSID %u turned off (OFF=)\n", nsids[i]);
#endif
            continue;
        }
        NvmeAddNamespace(DevExt, nsids[i]);
    }

    // Keep LUN 0 usable for SMART/IOCTLs even without a namespace
    return TRUE;
}

//
// NvmeSubmitAer - Keep one Asynchronous Event Request outstanding
//
VOID NvmeSubmitAer(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;

    if (!DevExt->AerOn || DevExt->AerOutstanding || DevExt->Stopped || !DevExt->InitComplete) {
        return;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_ASYNC_EVENT;
    cmd.CDW0.Fields.CommandId = ADMIN_CID_AER;
    if (NvmeSubmitCommand(DevExt, &DevExt->AdminQueue, &cmd)) {
        DevExt->AerOutstanding = TRUE;
    }
}

//
// NvmeStartAer - After the controller was set up: which events it reports
// (Asynchronous Event Configuration), then the first request. On with the
// hot-plug check (it reads the event's log page and asks again).
//
VOID NvmeStartAer(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;
    ULONG events = 0x1F;                        // SMART / health critical warnings

    DevExt->AerOutstanding = FALSE;
    DevExt->AerLogPending = FALSE;
    DevExt->AerOn = (BOOLEAN)(DevExt->HotPlug && !DevExt->DumpMode);
    if (!DevExt->AerOn) {
        return;
    }
    if (DevExt->Oaes & NVME_OAES_NS_ATTR) {
        events |= 0x100;
    }
    if (DevExt->Oaes & NVME_OAES_FW_ACTIVATE) {
        events |= 0x200;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
    cmd.CDW10 = NVME_FEAT_ASYNC_EVENT;
    cmd.CDW11 = events;
    NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS);
    NvmeSubmitAer(DevExt);
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: asynchronous events %03X, request %s\n", events,
                   DevExt->AerOutstanding ? "outstanding" : "not taken");
#endif
}

//
// NvmeAerReadLog - Timer: the log page of the last event (reading it clears
// the event, RAE = 0), what it means for the driver, the next request
//
VOID NvmeAerReadLog(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;
    UCHAR page;
    PUCHAR buffer;
    ULONG bytes = (DevExt->AerLog == 0x04) ? NVME_PAGE_SIZE : 512;

    page = AllocatePrpListPage(DevExt);
    if (page == 0xFF) {
        return;                                 // pool busy: the next check
    }
    buffer = (PUCHAR)GetPrpListPageVirtual(DevExt, page);
    memset(buffer, 0, NVME_PAGE_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.NSID = 0xFFFFFFFF;
    cmd.PRP1 = GetPrpListPagePhysical(DevExt, page).QuadPart;
    cmd.CDW10 = DevExt->AerLog | (((bytes / 4) - 1) << 16);
    DevExt->AerLogPending = FALSE;
    if (!NvmeAdminSync(DevExt, &cmd, NVMENT_HOTPLUG_ADMIN_MS) && !DevExt->SyncDone) {
        // No answer: the page may still be written, and no further events
        DevExt->AerOn = FALSE;
        return;
    }
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: event type %u info %02X log %02X\n", DevExt->AerType, DevExt->AerInfo, DevExt->AerLog);
#endif
    if (DevExt->AerType == NVME_AER_TYPE_NOTICE && DevExt->AerInfo == NVME_AER_NOTICE_NS_CHANGED) {
        DevExt->NsChangePending = TRUE;
    } else if (DevExt->AerType == NVME_AER_TYPE_SMART) {
        DevExt->HealthWarning = buffer[0];
    }
    FreePrpListPage(DevExt, page);
    NvmeSubmitAer(DevExt);
}

//
// NvmeHotPlugNamespaces - HOTPLUG=1: the active namespace list read again
// (into a page of the PRP list pool; the utility buffer is that pool once
// the controller runs). A namespace that is gone keeps its LUN without a
// namespace (requests fail); a new one gets the next free LUN, never one
// a namespace had before. TRUE when something changed.
//
BOOLEAN NvmeHotPlugNamespaces(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG nsids[NVMENT_MAX_LUNS * 2];
    ULONG count = 0, i, l, k;
    UCHAR page;
    PULONG buffer;
    BOOLEAN changed = FALSE;

    if (DevExt->Version < 0x00010100 || DevExt->NsListBroken) {
        return FALSE;                           // no active namespace list
    }
    page = AllocatePrpListPage(DevExt);
    if (page == 0xFF) {
        return FALSE;                           // pool busy: the next check
    }
    buffer = (PULONG)GetPrpListPageVirtual(DevExt, page);
    if (!NvmeIdentifyInto(DevExt, 0, NVME_CNS_ACTIVE_NS, buffer, GetPrpListPagePhysical(DevExt, page),
                          NVMENT_HOTPLUG_ADMIN_MS)) {
        if (!DevExt->SyncDone) {
            // No answer in time: the controller may still write the page,
            // so it stays taken, and the list is not read again
            DevExt->NsListBroken = TRUE;
            return FALSE;
        }
        FreePrpListPage(DevExt, page);
        return FALSE;
    }
    while (count < NVMENT_MAX_LUNS * 2 && buffer[count] != 0) {
        nsids[count] = buffer[count];
        count++;
    }

    // After a format or sanitize: a namespace whose size or format changed
    // is a new one (it gets a new LUN, as for any other namespace)
    if (DevExt->NsRefresh) {
        DevExt->NsRefresh = FALSE;
        for (l = 0; l < DevExt->LunCount; l++) {
            PNVME_LUN lun = &DevExt->Luns[l];
            PNVME_IDENTIFY_NAMESPACE ns = (PNVME_IDENTIFY_NAMESPACE)buffer;
            UCHAR f;
            if (lun->NamespaceId == 0) {
                continue;
            }
            if (!NvmeIdentifyInto(DevExt, lun->NamespaceId, NVME_CNS_NAMESPACE, buffer, GetPrpListPagePhysical(DevExt, page),
                                  NVMENT_HOTPLUG_ADMIN_MS)) {
                if (!DevExt->SyncDone) {
                    DevExt->NsListBroken = TRUE;
                    return changed;
                }
                continue;
            }
            f = (UCHAR)(ns->FormattedLbaSize & NVME_FLBAS_INDEX_MASK);
            if (ns->NamespaceSize == (lun->SizeInBlocks << lun->LbaShift) &&
                ns->LbaFormats[f].LbaDataSize == (UCHAR)(lun->BlockShift - lun->LbaShift) &&
                ns->LbaFormats[f].MetadataSize == (lun->MetaSize != 0 ? lun->MetaSize : (lun->PiAct ? 8 : 0))) {
                continue;
            }
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NSID %u changed its format (LUN %u)\n", lun->NamespaceId, l);
#endif
            lun->GoneNsid = lun->NamespaceId;
            lun->GoneSize = ns->NamespaceSize + 1;     // never matches: a new LUN
            lun->NamespaceId = 0;
            lun->SizeInBlocks = 0;
            changed = TRUE;
        }
        // the namespace list is read again into the same page below
        if (!NvmeIdentifyInto(DevExt, 0, NVME_CNS_ACTIVE_NS, buffer, GetPrpListPagePhysical(DevExt, page),
                              NVMENT_HOTPLUG_ADMIN_MS)) {
            if (!DevExt->SyncDone) {
                DevExt->NsListBroken = TRUE;
                return changed;
            }
            FreePrpListPage(DevExt, page);
            return changed;
        }
        count = 0;
        while (count < NVMENT_MAX_LUNS * 2 && buffer[count] != 0) {
            nsids[count] = buffer[count];
            count++;
        }
    }

    // Gone
    for (l = 0; l < DevExt->LunCount; l++) {
        if (DevExt->Luns[l].NamespaceId == 0) {
            continue;
        }
        for (i = 0; i < count && nsids[i] != DevExt->Luns[l].NamespaceId; i++) ;
        if (i < count) {
            continue;
        }
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NSID %u gone (LUN %u)\n", DevExt->Luns[l].NamespaceId, l);
#endif
        DevExt->Luns[l].GoneNsid = DevExt->Luns[l].NamespaceId;
        DevExt->Luns[l].GoneSize = DevExt->Luns[l].SizeInBlocks << DevExt->Luns[l].LbaShift;
        DevExt->Luns[l].NamespaceId = 0;
        DevExt->Luns[l].SizeInBlocks = 0;
        changed = TRUE;
    }

    // New
    for (i = 0; i < count; i++) {
        for (l = 0; l < DevExt->LunCount && DevExt->Luns[l].NamespaceId != nsids[i]; l++) ;
        if (l < DevExt->LunCount) {
            continue;
        }
        for (k = 0; k < DevExt->NsOffCount && DevExt->NsOff[k] != nsids[i]; k++) ;
        if (k < DevExt->NsOffCount) {
            continue;
        }
        if (!NvmeIdentifyInto(DevExt, nsids[i], NVME_CNS_NAMESPACE, buffer, GetPrpListPagePhysical(DevExt, page),
                              NVMENT_HOTPLUG_ADMIN_MS)) {
            if (!DevExt->SyncDone) {
                DevExt->NsListBroken = TRUE;    // see above: the page stays taken
                return changed;
            }
            continue;
        }
        // The same namespace back (NSID and size): its old LUN (Windows 2000
        // did not find a new LUN behind the gap the namespace left); any
        // other one: the next LUN, never one another namespace had (all
        // namespaces show the same INQUIRY data)
        for (l = 0; l < DevExt->LunCount; l++) {
            if (DevExt->Luns[l].NamespaceId == 0 && DevExt->Luns[l].GoneNsid == nsids[i] &&
                DevExt->Luns[l].GoneSize == ((PNVME_IDENTIFY_NAMESPACE)buffer)->NamespaceSize) {
                break;
            }
        }
        if (l == DevExt->LunCount && DevExt->LunCount >= NVMENT_MAX_LUNS) {
            continue;
        }
        if (NvmeAddNamespaceData(DevExt, nsids[i], (PNVME_IDENTIFY_NAMESPACE)buffer, l)) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NSID %u arrived (LUN %u)\n", nsids[i], l);
#endif
            changed = TRUE;
        }
    }
    FreePrpListPage(DevExt, page);
    return changed;
}

//
// NvmeIdentifyEx - Send Identify command with custom parameters
//
BOOLEAN NvmeIdentifyEx(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN ULONG NamespaceId,
    IN ULONG CNS,
    IN USHORT CommandId,
    IN PSCSI_REQUEST_BLOCK Srb)
{
    NVME_COMMAND cmd;
    PVOID prpPageVirt;
    UCHAR prpPageIndex;
    PNVME_SRB_EXTENSION srbExt;

    // Store PRP page index in SRB extension so we can free it on completion
    if (!Srb->SrbExtension) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeIdentifyEx - SRB has no extension\n");
#endif
        return FALSE;
    }
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;

    // Allocate a PRP page for the identify data buffer (4KB)
    prpPageIndex = AllocatePrpListPage(DevExt);
    if (prpPageIndex == 0xFF) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeIdentifyEx - Failed to allocate PRP page\n");
#endif
        return FALSE;
    }
    srbExt->PrpListPage = prpPageIndex;

    // Zero out the page to prevent stale data issues
    prpPageVirt = GetPrpListPageVirtual(DevExt, prpPageIndex);
    memset(prpPageVirt, 0, NVME_PAGE_SIZE);

    memset(&cmd, 0, sizeof(NVME_COMMAND));

    // Build Identify command
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_IDENTIFY;
    cmd.CDW0.Fields.Flags = 0;
    cmd.CDW0.Fields.CommandId = CommandId;
    cmd.NSID = NamespaceId;
    cmd.PRP1 = GetPrpListPagePhysical(DevExt, prpPageIndex).QuadPart;
    cmd.PRP2 = 0;  // Single page transfer, no PRP2 needed
    cmd.CDW10 = CNS;

#ifdef NVMENT_DBG_CMD
    ScsiDebugPrint(0, "NVMENT: NvmeIdentifyEx - CDW0=%08X (OPC=%02X CID=%04X) NSID=%08X PRP1=%08X%08X CDW10=%08X\n",
                   cmd.CDW0.AsUlong, cmd.CDW0.Fields.Opcode, cmd.CDW0.Fields.CommandId,
                   cmd.NSID, (ULONG)(cmd.PRP1 >> 32), (ULONG)(cmd.PRP1 & 0xFFFFFFFF),
                   cmd.CDW10);
#endif

    if (CommandId & CID_NON_TAGGED_FLAG) {
        DevExt->Luns[CID_LUN(CommandId)].NonTaggedInFlight = Srb;
    }
    if (!NvmeSubmitAdminCommand(DevExt, &cmd)) {
        if (CommandId & CID_NON_TAGGED_FLAG) {
            DevExt->Luns[CID_LUN(CommandId)].NonTaggedInFlight = NULL;
        }
        FreePrpListPage(DevExt, prpPageIndex);
        srbExt->PrpListPage = 0xFF;
        return FALSE;
    } else {
        return TRUE;
    }
}

//
// NvmeGetLogPage - Retrieve a log page from NVMe device asynchronously
// Uses PRP page allocator for DMA buffer, untagged operation
// ScsiPort guarantees only one untagged request at a time
//
BOOLEAN NvmeGetLogPage(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR LogPageId)
{
    NVME_COMMAND cmd;
    PVOID prpPageVirt;
    UCHAR prpPageIndex;
    ULONG numdl;
    PNVME_SRB_EXTENSION srbExt;

    // Store PRP page index in SRB extension so we can free it on completion
    if (!Srb->SrbExtension) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeGetLogPage - SRB has no extension\n");
#endif
        return FALSE;
    }
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;

    // Allocate a PRP page for the log data buffer (4KB)
    prpPageIndex = AllocatePrpListPage(DevExt);
    if (prpPageIndex == 0xFF) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeGetLogPage - Failed to allocate PRP page\n");
#endif
        return FALSE;
    }
    srbExt->PrpListPage = prpPageIndex;

    // Zero out the page to prevent stale data issues
    prpPageVirt = GetPrpListPageVirtual(DevExt, prpPageIndex);
    memset(prpPageVirt, 0, NVME_PAGE_SIZE);

    memset(&cmd, 0, sizeof(NVME_COMMAND));

    cmd.CDW0.Fields.Opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.CDW0.Fields.CommandId = NvmeBuildAdminCommandId(DevExt, Srb, ADMIN_KIND_GET_LOG_PAGE);
    cmd.NSID = 0xFFFFFFFF;  // Global log page (not namespace-specific)
    cmd.PRP1 = GetPrpListPagePhysical(DevExt, prpPageIndex).QuadPart;
    cmd.PRP2 = 0;  // Single page transfer

    // Calculate NUMDL (number of dwords - 1) for 512 bytes (SMART log size)
    numdl = (512 / 4) - 1;
    // Bits 31:16 = NUMDL (Number of Dwords Lower)
    // Bits 15:08 = Reserved
    // Bits 07:00 = LID (Log Page Identifier)
    cmd.CDW10 = (LogPageId & 0xFF) | ((numdl & 0xFFFF) << 16);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeGetLogPage - LID=0x%02X PRP=%u Phys=%08X%08X\n",
                   LogPageId, prpPageIndex, (ULONG)(cmd.PRP1 >> 32), (ULONG)(cmd.PRP1));
#endif

    // ScsiPort manages the untagged SRB, we can retrieve it in completion with ScsiPortGetSrb()
    if (cmd.CDW0.Fields.CommandId & CID_NON_TAGGED_FLAG) {
        DevExt->Luns[NVME_LUN_INDEX(Srb)].NonTaggedInFlight = Srb;
    }
    if (!NvmeSubmitAdminCommand(DevExt, &cmd)) {
        if (cmd.CDW0.Fields.CommandId & CID_NON_TAGGED_FLAG) {
            DevExt->Luns[NVME_LUN_INDEX(Srb)].NonTaggedInFlight = NULL;
        }
        FreePrpListPage(DevExt, prpPageIndex);
        srbExt->PrpListPage = 0xFF;
        return FALSE;
    } else {
        return TRUE;
    }
}

//
// NvmeGetLogPageEx - Retrieve a log page with custom parameters
//
BOOLEAN NvmeGetLogPageEx(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN UCHAR LogPageId,
    IN ULONG NamespaceId,
    IN USHORT CommandId,
    IN ULONG NumDwords)
{
    NVME_COMMAND cmd;
    PVOID prpPageVirt;
    UCHAR prpPageIndex;
    ULONG numdl;
    PNVME_SRB_EXTENSION srbExt;

    // Store PRP page index in SRB extension so we can free it on completion
    if (!Srb->SrbExtension) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeGetLogPageEx - SRB has no extension\n");
#endif
        return FALSE;
    }
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;

    // Allocate a PRP page for the log data buffer (4KB)
    prpPageIndex = AllocatePrpListPage(DevExt);
    if (prpPageIndex == 0xFF) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeGetLogPageEx - Failed to allocate PRP page\n");
#endif
        return FALSE;
    }
    srbExt->PrpListPage = prpPageIndex;

    // Zero out the page to prevent stale data issues
    prpPageVirt = GetPrpListPageVirtual(DevExt, prpPageIndex);
    memset(prpPageVirt, 0, NVME_PAGE_SIZE);

    memset(&cmd, 0, sizeof(NVME_COMMAND));

    cmd.CDW0.Fields.Opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.CDW0.Fields.CommandId = CommandId;
    cmd.NSID = NamespaceId;
    cmd.PRP1 = GetPrpListPagePhysical(DevExt, prpPageIndex).QuadPart;
    cmd.PRP2 = 0;  // Single page transfer

    // NUMDL = number of dwords minus 1
    // If NumDwords is 0, default to full page (1024 DWORDs = 4096 bytes)
    if (NumDwords == 0) {
        numdl = (NVME_PAGE_SIZE >> 2) - 1;  // 1023 (0x3FF) for 4KB
    } else {
        numdl = NumDwords - 1;
    }

    // Bits 31:16 = NUMDL (Number of Dwords Lower)
    // Bits 15:08 = Reserved
    // Bits 07:00 = LID (Log Page Identifier)
    cmd.CDW10 = (LogPageId & 0xFF) | ((numdl & 0xFFFF) << 16);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeGetLogPageEx - LID=0x%02X NSID=%08X CID=%04X NUMDL=%u PRP=%u Phys=%08X%08X\n",
                   LogPageId, NamespaceId, CommandId, numdl, prpPageIndex, (ULONG)(cmd.PRP1 >> 32), (ULONG)(cmd.PRP1));
#endif

    if (CommandId & CID_NON_TAGGED_FLAG) {
        DevExt->Luns[CID_LUN(CommandId)].NonTaggedInFlight = Srb;
    }
    if (!NvmeSubmitAdminCommand(DevExt, &cmd)) {
        if (CommandId & CID_NON_TAGGED_FLAG) {
            DevExt->Luns[CID_LUN(CommandId)].NonTaggedInFlight = NULL;
        }
        FreePrpListPage(DevExt, prpPageIndex);
        srbExt->PrpListPage = 0xFF;
        return FALSE;
    } else {
        return TRUE;
    }
}

//
// NvmeBuildCommandId - Build NVMe Command ID from SRB
// For tagged requests: QueueTag (bit 15 clear)
// For non-tagged requests: sequence number with bit 15 set
// Both carry the logical unit index in bits 8-12
//
USHORT NvmeBuildCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    USHORT commandId;
    USHORT lunBits = (USHORT)((NVME_LUN_INDEX(Srb) << CID_LUN_SHIFT) & CID_LUN_MASK);

    // Check if this is a tagged request
    // Note: QueueTag == SP_UNTAGGED (0xFF) means non-tagged even if SRB_FLAGS_QUEUE_ACTION_ENABLE is set
    if ((Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) && (Srb->QueueTag != SP_UNTAGGED)) {
        // Tagged request - use QueueTag directly
        commandId = (USHORT)(((USHORT)Srb->QueueTag & CID_TAG_MASK) | lunBits);
    } else {
        // Non-tagged request - generate sequence number with flag bit set
        commandId = (USHORT)((DevExt->NextNonTaggedId & CID_TAG_MASK) | lunBits | CID_NON_TAGGED_FLAG);
        DevExt->NextNonTaggedId++;
    }

    return commandId;
}

//
// NvmeFreeSrbPages - Give back the PRP list and metadata pages of a request
//
VOID NvmeFreeSrbPages(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_SRB_EXTENSION SrbExt)
{
    if (SrbExt->PrpListPage != 0xFF) {
        FreePrpListPage(DevExt, SrbExt->PrpListPage);
        SrbExt->PrpListPage = 0xFF;
    }
    if (SrbExt->MetaPage != 0xFF) {
        FreePrpListPage(DevExt, SrbExt->MetaPage);
        SrbExt->MetaPage = 0xFF;
    }
}

//
// NvmeBuildAdminCommandId - Command ID for an admin command carrying an SRB
//
USHORT NvmeBuildAdminCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Kind)
{
    return (USHORT)(NvmeBuildCommandId(DevExt, Srb) | ((Kind << CID_KIND_SHIFT) & CID_KIND_MASK));
}

//
// NvmeBuildFlushCommandId - Build CID for ORDERED tag flush command
// Uses the SRB's QueueTag with the ORDERED_FLUSH flag bit set
//
USHORT NvmeBuildFlushCommandId(IN PSCSI_REQUEST_BLOCK Srb)
{
    USHORT commandId;

    // For ORDERED flush, use QueueTag with flush flag bit set
    commandId = (USHORT)(((USHORT)Srb->QueueTag & CID_TAG_MASK) |
                         ((NVME_LUN_INDEX(Srb) << CID_LUN_SHIFT) & CID_LUN_MASK) |
                         CID_ORDERED_FLUSH_FLAG);

    return commandId;
}

//
// NvmeGetSrbFromCommandId - Retrieve SRB from Command ID
// Decodes the CID and calls ScsiPortGetSrb with appropriate parameters
//
PSCSI_REQUEST_BLOCK NvmeGetSrbFromCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT commandId)
{
    LONG queueTag;
    PSCSI_REQUEST_BLOCK Srb;
    UCHAR lun = CID_LUN(commandId);

    // Check if this is a non-tagged request (bit 15 set)
    if (commandId & CID_NON_TAGGED_FLAG) {
        // Non-tagged request - use SP_UNTAGGED
        queueTag = SP_UNTAGGED;
    } else if (commandId & CID_ORDERED_FLUSH_FLAG) {
        // There is no actual SRB for the flush, the next command has SRB
        return NULL;
    } else {
        // Normal tagged request - extract QueueTag
        queueTag = (LONG)(commandId & CID_TAG_MASK);
    }

    Srb = ScsiPortGetSrb(DevExt, 0, (UCHAR)(lun / NVMENT_LUNS_PER_TARGET), (UCHAR)(lun % NVMENT_LUNS_PER_TARGET), queueTag);
    if (queueTag == SP_UNTAGGED) {
        if (!Srb)
            Srb = DevExt->Luns[lun].NonTaggedInFlight;
        DevExt->Luns[lun].NonTaggedInFlight = NULL;
    }
    return Srb;
}

//
// NvmeBuildReadWriteCommand - Build NVMe Read/Write command from SCSI CDB
//
int NvmeBuildReadWriteCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PNVME_COMMAND Cmd, IN USHORT CommandId)
{
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    ULONGLONG lba = 0;
    ULONG numBlocks = 0;
    BOOLEAN isWrite = FALSE;
    BOOLEAN fua = FALSE;
    PHYSICAL_ADDRESS physAddr;
    PHYSICAL_ADDRESS physAddr2;
    ULONG length;
    ULONG offsetInPage;
    ULONG firstPageBytes;
    PVOID currentPageVirtual;
    ULONG remainingBytes;
    ULONG currentOffset;
    UCHAR prpListPage;
    PULONGLONG prpList;
    PHYSICAL_ADDRESS prpListPhys;
    ULONG prpIndex;
    ULONG numPrpEntries;
    PNVME_SRB_EXTENSION srbExt;

    // Initialize SRB extension
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
    srbExt->PrpListPage = 0xFF;  // No PRP list initially

    ScsiGetLbaCount(Srb, &lba, &numBlocks, &isWrite, &fua);

    // validate against buffer size
    if (numBlocks > (Srb->DataTransferLength >> lun->BlockShift)) {
        ScsiDebugPrint(0, "NVMENT: Transfer size in blocks %u exceeds buffer size %u - rejecting\n",
                       numBlocks, Srb->DataTransferLength);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Transfer size in blocks %u exceeds buffer size %u - rejecting\n",
                       numBlocks, Srb->DataTransferLength);
#endif
        DevExt->RejectedRequests++;
        lun->NonTaggedInFlight = NULL;
        ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
        return -1;
    }
    // Validate transfer size against MDTS
    if (numBlocks > (DevExt->MaxTransferSizeBytes >> lun->BlockShift)) {
        ScsiDebugPrint(0, "NVMENT: Transfer size in blocks %u exceeds MDTS limit %u - rejecting\n",
                       numBlocks, DevExt->MaxTransferSizeBytes);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Transfer size in blocks %u exceeds MDTS limit %u - rejecting\n",
                       numBlocks, DevExt->MaxTransferSizeBytes);
#endif
        DevExt->RejectedRequests++;
        lun->NonTaggedInFlight = NULL;
        ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
        return -1;
    }

    // Build NVMe command
    if (isWrite) {
        Cmd->CDW0.Fields.Opcode = NVME_CMD_WRITE;
    } else {
        Cmd->CDW0.Fields.Opcode = NVME_CMD_READ;
    }
    Cmd->CDW0.Fields.Flags = NVME_CMD_PRP;
    Cmd->CDW0.Fields.CommandId = CommandId;

    Cmd->NSID = lun->NamespaceId;

    // Track I/O statistics
    DevExt->TotalRequests++;
    if (isWrite) {
        DevExt->TotalWrites++;
        DevExt->TotalBytesWritten += Srb->DataTransferLength;
        if (Srb->DataTransferLength > DevExt->MaxWriteSize) {
            DevExt->MaxWriteSize = Srb->DataTransferLength;
        }
    } else {
        DevExt->TotalReads++;
        DevExt->TotalBytesRead += Srb->DataTransferLength;
        if (Srb->DataTransferLength > DevExt->MaxReadSize) {
            DevExt->MaxReadSize = Srb->DataTransferLength;
        }
    }
#if NVMENT_DBG_STATS
    // Print statistics every 10000 requests
    // Note: QDepth tracking removed (always 0) since we no longer store SRBs
    if ((DevExt->TotalRequests % 10000) == 0) {
        ScsiDebugPrint(0, "NVMENT: Stats - Reqs=%u R=%u W=%u BytesR=%I64u BytesW=%I64u MaxR=%u MaxW=%u PRP=%u/%u Rejected=%u\n",
                       DevExt->TotalRequests,
                       DevExt->TotalReads,
                       DevExt->TotalWrites,
                       DevExt->TotalBytesRead,
                       DevExt->TotalBytesWritten,
                       DevExt->MaxReadSize,
                       DevExt->MaxWriteSize,
                       DevExt->CurrentPrpListPagesUsed,
                       DevExt->MaxPrpListPagesUsed,
                       DevExt->RejectedRequests);
    }
#endif

    // Check for TRIM mode: if writing and TRIM is enabled, compare first 4KB with pattern
    if (isWrite && DevExt->TrimEnable && DevExt->TrimPattern != NULL && Srb->DataTransferLength >= 4096) {
        // Compare first 4KB of DataBuffer with TrimPattern
        if (memcmp(Srb->DataBuffer, DevExt->TrimPattern, 4096) == 0) {
            // Match! Convert to TRIM/UNMAP (Dataset Management) command
#ifdef NVMENT_DBG_EXTRA
            ScsiDebugPrint(0, "NVMENT: TRIM pattern detected at LBA %08X%08X, blocks=%u - converting to DSM\n",
                           (ULONG)(lba >> 32), (ULONG)(lba & 0xFFFFFFFF), numBlocks);
#endif
            // Build Dataset Management command
            Cmd->CDW0.Fields.Opcode = NVME_CMD_DSM;
            // Flags and CommandId already set, NSID already set

            // Clear reserved fields
            Cmd->CDW2 = 0;
            Cmd->CDW3 = 0;
            Cmd->MPTR = 0;

            // DSM command format:
            // CDW10: NR (Number of Ranges) - 1 range = 0
            // CDW11: Attributes - bit 2 (AD) = deallocate
            // PRP points to range data: [Context Attributes (32bit)][Length in blocks (32bit)][Starting LBA (64bit)]
            Cmd->CDW10 = 0;  // 0 = 1 range
            Cmd->CDW11 = (1 << 2);  // AD (Attribute - Deallocate)
            Cmd->CDW12 = 0;
            Cmd->CDW13 = 0;
            Cmd->CDW14 = 0;
            Cmd->CDW15 = 0;

            // Build range data structure in data buffer (reuse the buffer)
            // Format: [Context Attributes: 32 bits][Length: 32 bits][SLBA: 64 bits]
            {
                PULONG rangeData = (PULONG)Srb->DataBuffer;
                rangeData[0] = 0;  // Context attributes
                rangeData[1] = numBlocks << lun->LbaShift;  // Length in namespace blocks
                rangeData[2] = (ULONG)((lba << lun->LbaShift) & 0xFFFFFFFF);  // Starting LBA (low)
                rangeData[3] = (ULONG)((lba << lun->LbaShift) >> 32);  // Starting LBA (high)
#ifdef NVMENT_DBG_EXTRA
                ScsiDebugPrint(0, "NVMENT: DSM range data - CtxAttr=0x%08X Len=%u SLBA=0x%08X%08X\n",
                               rangeData[0], rangeData[1], rangeData[3], rangeData[2]);
#endif
            }

            // Get physical address for the range data (only need 16 bytes)
            length = 16;
            physAddr = ScsiPortGetPhysicalAddress(DevExt, Srb, Srb->DataBuffer, &length);
            if (physAddr.QuadPart == 0) {
                DevExt->RejectedRequests++;
                lun->NonTaggedInFlight = NULL;
                ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
                return -1;
            }

            Cmd->PRP1 = physAddr.QuadPart;
            Cmd->PRP2 = 0;

#ifdef NVMENT_DBG_EXTRA
            ScsiDebugPrint(0, "NVMENT: DSM command - CDW10=0x%08X CDW11=0x%08X PRP1=0x%08X%08X\n",
                           Cmd->CDW10, Cmd->CDW11,
                           (ULONG)(Cmd->PRP1 >> 32), (ULONG)(Cmd->PRP1 & 0xFFFFFFFF));
#endif

            return 1;
        }
    }

    // Normal read/write: Set LBA and number of blocks
    // In namespace blocks (4K=1: eight per SCSI block)
    lba <<= lun->LbaShift;
    numBlocks <<= lun->LbaShift;
    Cmd->CDW10 = (ULONG)(lba & 0xFFFFFFFF);
    Cmd->CDW11 = (ULONG)(lba >> 32);
    Cmd->CDW12 = ((numBlocks > 0) ? (numBlocks - 1) : 0) | (fua ? NVME_RW_FUA : 0);
    Cmd->CDW13 = 0;
    Cmd->CDW14 = 0;
    Cmd->CDW15 = 0;
    if (lun->PiAct) {
        Cmd->CDW12 |= NVME_RW_PRACT;
    }
    if (lun->MetaSize != 0) {
        // Metadata the system has no use for: zeros on write, dropped on read
        PNVME_SRB_EXTENSION ext = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
        UCHAR metaPage;
        if (numBlocks * lun->MetaSize > NVME_PAGE_SIZE) {
            DevExt->RejectedRequests++;
            lun->NonTaggedInFlight = NULL;
            ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
            return -1;
        }
        metaPage = AllocatePrpListPage(DevExt);
        if (metaPage == 0xFF) {
            lun->NonTaggedInFlight = NULL;
            return 0;
        }
        if (isWrite) {
            memset(GetPrpListPageVirtual(DevExt, metaPage), 0, numBlocks * lun->MetaSize);
        }
        ext->MetaPage = metaPage;
        Cmd->MPTR = GetPrpListPagePhysical(DevExt, metaPage).QuadPart;
    }

    // Normal read/write path - build PRPs
    // Get physical address of data buffer
    length = Srb->DataTransferLength;
    physAddr = ScsiPortGetPhysicalAddress(DevExt, Srb, Srb->DataBuffer, &length);
    if (physAddr.QuadPart == 0) {
        DevExt->RejectedRequests++;
        lun->NonTaggedInFlight = NULL;
        ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
        return -1;
    }

#ifdef NVMENT_DBG_CMD
    ScsiDebugPrint(0, "NVMENT: NvmeBuildReadWriteCommand - DataBuffer=%p TransferLen=%u PhysAddr=%08X%08X ReturnedLen=%u\n",
                   Srb->DataBuffer, Srb->DataTransferLength,
                   (ULONG)(physAddr.QuadPart >> 32), (ULONG)(physAddr.QuadPart & 0xFFFFFFFF),
                   length);
#endif
    // Set PRP1 to the start of the data
    Cmd->PRP1 = physAddr.QuadPart;

    // Calculate offset within the page
    offsetInPage = (ULONG)(physAddr.QuadPart & NVME_PAGE_MASK);

    // Calculate how many bytes fit in the first page
    firstPageBytes = NVME_PAGE_SIZE - offsetInPage;

    // Determine if we need PRP2 or a PRP list
    if (Srb->DataTransferLength <= firstPageBytes) {
        // Transfer fits in one page
        Cmd->PRP2 = 0;
#ifdef NVMENT_DBG_CMD
        ScsiDebugPrint(0, "NVMENT: NvmeBuildReadWriteCommand - Single page transfer, PRP2=0\n");
#endif
    } else if (Srb->DataTransferLength <= (firstPageBytes + NVME_PAGE_SIZE)) {
        // Transfer spans exactly 2 pages, use PRP2 directly
        currentPageVirtual = (PVOID)((PUCHAR)Srb->DataBuffer + firstPageBytes);
        length = Srb->DataTransferLength - firstPageBytes;
        physAddr2 = ScsiPortGetPhysicalAddress(DevExt, Srb, currentPageVirtual, &length);
        if (physAddr2.QuadPart == 0) {
            DevExt->RejectedRequests++;
            lun->NonTaggedInFlight = NULL;
            ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
            return -1;
        }

#ifdef NVMENT_DBG_CMD
        ScsiDebugPrint(0, "NVMENT: NvmeBuildReadWriteCommand - Two page transfer: PhysAddr2=%08X%08X\n",
                       (ULONG)(physAddr2.QuadPart >> 32), (ULONG)(physAddr2.QuadPart & 0xFFFFFFFF));
#endif
        Cmd->PRP2 = physAddr2.QuadPart;
    } else {
        // Transfer spans more than 2 pages, need PRP list
        prpListPage = AllocatePrpListPage(DevExt);
        if (prpListPage == 0xFF) {
            // No PRP list pages available - this shouldn't happen if we sized correctly
            ScsiDebugPrint(0, "NVMENT: No PRP list pages available %d/%d!\n", DevExt->CurrentPrpListPagesUsed, DevExt->SgListPages);
            Cmd->PRP2 = 0;
            DevExt->RejectedRequests++;
            lun->NonTaggedInFlight = NULL;
            return 0;
        }

        // Store PRP list page in SRB extension
        {
            PNVME_SRB_EXTENSION srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
            srbExt->PrpListPage = prpListPage;
        }

        // Get virtual and physical addresses of PRP list
        prpList = (PULONGLONG)GetPrpListPageVirtual(DevExt, prpListPage);
        prpListPhys = GetPrpListPagePhysical(DevExt, prpListPage);

        // Build PRP list for remaining pages
        remainingBytes = Srb->DataTransferLength - firstPageBytes;
        currentOffset = firstPageBytes;
        prpIndex = 0;

        while (remainingBytes > 0 && prpIndex < 512) {
            currentPageVirtual = (PVOID)((PUCHAR)Srb->DataBuffer + currentOffset);
            length = remainingBytes;
            physAddr2 = ScsiPortGetPhysicalAddress(DevExt, Srb, currentPageVirtual, &length);
            if (physAddr2.QuadPart == 0) {
                FreePrpListPage(DevExt, prpListPage);
                DevExt->RejectedRequests++;
                lun->NonTaggedInFlight = NULL;
                ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
                return -1;
            }

            prpList[prpIndex] = physAddr2.QuadPart;
            prpIndex++;

            if (remainingBytes <= NVME_PAGE_SIZE) {
                break;
            }

            remainingBytes -= NVME_PAGE_SIZE;
            currentOffset += NVME_PAGE_SIZE;
        }

        numPrpEntries = prpIndex;

#ifdef NVMENT_DBG_CMD
        ScsiDebugPrint(0, "NVMENT: NvmeBuildReadWriteCommand - PRP list: page=%u entries=%u listPhys=%08X%08X\n",
                       prpListPage, numPrpEntries,
                       (ULONG)(prpListPhys.QuadPart >> 32), (ULONG)(prpListPhys.QuadPart & 0xFFFFFFFF));
#endif

        // Set PRP2 to point to the PRP list
        Cmd->PRP2 = prpListPhys.QuadPart;
    }

    // CDW10-15 already set above before TRIM check
    return 1;
}

//
// NvmeShutdownController - Perform clean shutdown of NVMe controller
// Deletes I/O queues, issues shutdown notification, and disables controller
// Robust version that handles partially initialized or unknown controller state
//
//
// NvmePowerDown - queued commands finished, then the shutdown notification
// (the controller writes its volatile cache back). Used for the power IOCTL
// and SRB_FUNCTION_SHUTDOWN; the next request sets the controller up again.
//
VOID NvmePowerDown(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG wait = 0;

    if (DevExt->Stopped) return;
    while ((DevExt->CurrentQueueDepth != 0) && wait++ < 3000000) {
        if (NvmeReadReg32(DevExt, NVME_REG_CSTS) == 0xFFFFFFFF) break;   // gone: nothing completes
        NvmeProcessIoCompletion(DevExt);
        NvmeProcessAdminCompletion(DevExt);
        ScsiPortStallExecution(10);
    }
    NvmeFullPower(DevExt);
    NvmeShutdownController(DevExt);
}

VOID NvmeShutdownController(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG cc, csts;
    ULONG timeoutMs = 5000;  // 5 second timeout
    ULONG elapsed = 0;
    ULONG shutdownStatus;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeShutdownController - starting shutdown sequence\n");
#endif

    // First, mask all interrupts to prevent interrupt storms during shutdown
    NvmeWriteReg32(DevExt, NVME_REG_INTMS, 0xFFFFFFFF);
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeShutdownController - masked all interrupts\n");
#endif

    // Check if controller is even enabled before trying to delete queues
    // (all ones: powered off or gone, nothing to wait for)
    csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
    if (csts == 0xFFFFFFFF || !(csts & NVME_CSTS_RDY)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeShutdownController - controller not ready, skipping queue deletion\n");
#endif
        // Controller is already disabled, just clean up state and return
        goto cleanup_state;
    }

    // Step 1: Delete I/O Submission Queue (must be deleted before CQ)
    if (DevExt->InitComplete && DevExt->IoQueue.SubmissionQueue != NULL) {
        NVME_COMMAND cmd;

        memset(&cmd, 0, sizeof(NVME_COMMAND));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_DELETE_SQ;
        cmd.CDW0.Fields.CommandId = ADMIN_CID_SHUTDOWN_DELETE_SQ;
        cmd.CDW10 = 1;  // QID = 1 (I/O queue)

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Deleting I/O Submission Queue\n");
#endif
        NvmeSubmitAdminCommand(DevExt, &cmd);

        // Poll for completion (simple polling, no interrupt)
        timeoutMs = 1000; // 1 second timeout
        elapsed = 0;
        while (elapsed < timeoutMs) {
            if (NvmeProcessAdminCompletion(DevExt)) {
                break;
            }
            ScsiPortStallExecution(1000);  // 1 millisecond
            elapsed++;
        }
    }

    // Step 2: Delete I/O Completion Queue
    if (DevExt->InitComplete && DevExt->IoQueue.CompletionQueue != NULL) {
        NVME_COMMAND cmd;

        memset(&cmd, 0, sizeof(NVME_COMMAND));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_DELETE_CQ;
        cmd.CDW0.Fields.CommandId = ADMIN_CID_SHUTDOWN_DELETE_CQ;
        cmd.CDW10 = 1;  // QID = 1 (I/O queue)

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Deleting I/O Completion Queue\n");
#endif
        NvmeSubmitAdminCommand(DevExt, &cmd);

        // Poll for completion
        timeoutMs = 1000; // 1 second timeout
        elapsed = 0;
        while (elapsed < timeoutMs) {
            if (NvmeProcessAdminCompletion(DevExt)) {
                break;
            }
            ScsiPortStallExecution(1000); // 1 millisecond
            elapsed++;
        }
    }

    // Step 3: Issue shutdown notification
    cc = NvmeReadReg32(DevExt, NVME_REG_CC);

    // Clear existing shutdown bits and set normal shutdown
    cc = (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL;
    NvmeWriteReg32(DevExt, NVME_REG_CC, cc);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Shutdown notification sent, waiting for completion\n");
#endif

    // Step 4: Wait for shutdown to complete (CSTS.SHST = 10b)
    timeoutMs = 5000; // 5 second timeout
    elapsed = 0;
    while (elapsed < timeoutMs) {
        csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
        shutdownStatus = csts & NVME_CSTS_SHST_MASK;

        if (shutdownStatus == NVME_CSTS_SHST_COMPLETE) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Shutdown complete\n");
#endif
            break;
        }
        ScsiPortStallExecution(1000); // 1 millisecond
        elapsed++;
    }

    if (elapsed >= timeoutMs) {
        ScsiDebugPrint(0, "NVMENT: WARNING - Shutdown timeout, CSTS=0x%08X\n", csts);
        // Controller may not be in a clean state, but we must continue shutdown
    }

    // Step 6: Disable the controller (CC.EN = 0)
    cc = NvmeReadReg32(DevExt, NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    NvmeWriteReg32(DevExt, NVME_REG_CC, cc);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Controller disabled\n");
#endif

    // Step 7: Wait for controller to become not ready (CSTS.RDY = 0)
    // This is critical - controller must be fully stopped before driver unload
    if (!NvmeWaitForReady(DevExt, FALSE)) {
        csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
        ScsiDebugPrint(0, "NVMENT: WARNING - Controller failed to clear RDY bit, CSTS=0x%08X\n", csts);
        // Continue anyway - we've done what we can
    }
#ifdef NVMENT_DBG
    else {
        ScsiDebugPrint(0, "NVMENT: Controller RDY bit cleared\n");
    }
#endif

    // Step 5 (Moved): Clear admin queue hardware registers AFTER disabling controller
    // This ensures the controller doesn't try to access stale queue pointers during shutdown
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Clearing admin queue registers\n");
#endif

    NvmeWriteReg32(DevExt, NVME_REG_AQA, 0);
    NvmeWriteReg64(DevExt, NVME_REG_ASQ, 0);
    NvmeWriteReg64(DevExt, NVME_REG_ACQ, 0);

cleanup_state:
    // Step 8: Reset queue software state to match hardware reset state
    // When controller is disabled (CC.EN=0), hardware resets doorbell registers to 0
    // We must reset our software state to match, otherwise re-initialization will fail
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Resetting queue state\n");
#endif

    // Reset Admin Queue state
    DevExt->AdminQueue.SubmissionQueueHead = 0;
    DevExt->AdminQueue.SubmissionQueueTail = 0;
    // Reset CQ head to QueueSize to re-establish phase bit as 1 for next init
    DevExt->AdminQueue.CompletionQueueHead = DevExt->AdminQueue.QueueSize;
    DevExt->AdminQueue.CompletionQueueTail = 0;

    // Reset I/O Queue state
    DevExt->IoQueue.SubmissionQueueHead = 0;
    DevExt->IoQueue.SubmissionQueueTail = 0;
    // Reset CQ head to QueueSize to re-establish phase bit as 1 for next init
    DevExt->IoQueue.CompletionQueueHead = DevExt->IoQueue.QueueSize;
    DevExt->IoQueue.CompletionQueueTail = 0;

    // Clear init state; the next request (or ScsiRestartAdapter / POWER_UP) sets it up again
    DevExt->InitComplete = FALSE;
    DevExt->Stopped = TRUE;
    DevExt->PowerState = 0;
    {
        ULONG l;
        for (l = 0; l < NVMENT_MAX_LUNS; l++) {
            DevExt->Luns[l].NonTaggedInFlight = NULL;
        }
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Shutdown sequence complete\n");
#endif
}

//
// NvmeSanitizeController - Sanitize and disable the NVMe controller
// This function handles cleanup of any residual state from previous drivers/option ROMs
//
BOOLEAN NvmeSanitizeController(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG cc, csts;
    int retryCount;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController called\n");
#endif

    //
    // NUCLEAR OPTION: Assume the NVMe option ROM or previous driver left the controller
    // in an unknown state, possibly with queues configured and interrupts firing.
    // We need to completely reset everything before we start.
    //

    // Step 1: MASK ALL INTERRUPTS IMMEDIATELY to stop any interrupt storm
    // This is critical - do this BEFORE reading any other registers or state
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - masking all interrupts\n");
#endif
    NvmeWriteReg32(DevExt, NVME_REG_INTMS, 0xFFFFFFFF);  // Mask all 32 interrupt vectors

    // Step 2: Check current controller state
    csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
    cc = NvmeReadReg32(DevExt, NVME_REG_CC);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - controller state before reset: CC=%08X CSTS=%08X\n", cc, csts);
    if (csts & NVME_CSTS_RDY) {
        ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - WARNING: Controller is READY (option ROM left it enabled!)\n");
    }
    if (csts & NVME_CSTS_CFS) {
        ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - WARNING: Controller Fatal Status bit set!\n");
    }
#endif

    // Step 3: Clear admin queue registers BEFORE disabling controller
    // This prevents the controller from trying to access stale queue memory
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - clearing admin queue registers\n");
#endif
    NvmeWriteReg32(DevExt, NVME_REG_AQA, 0);
    NvmeWriteReg64(DevExt, NVME_REG_ASQ, 0);
    NvmeWriteReg64(DevExt, NVME_REG_ACQ, 0);

    //
    // Step 4: Force controller disable, regardless of current state
    // Retry multiple times if needed - the option ROM may have left it in a weird state
    //
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - disabling controller\n");
#endif
    retryCount = 5; // Try up to 5 times with increasing aggression
    while (retryCount > 0) {
        // Clear CC.EN and CC.SHN (shutdown notification) bits
        cc = NvmeReadReg32(DevExt, NVME_REG_CC);
        cc &= ~(NVME_CC_ENABLE | NVME_CC_SHN_MASK);
        NvmeWriteReg32(DevExt, NVME_REG_CC, cc);

        // Wait for controller to become not ready
        if (NvmeWaitForReady(DevExt, FALSE)) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - controller disabled successfully\n");
#endif
            break; // Controller is disabled, proceed
        }

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - controller disable retry %d/5\n", 6 - retryCount);
#endif
        retryCount--;

        // On the last retry, try writing 0 to CC entirely (nuclear option)
        if (retryCount == 1) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - trying nuclear option: writing 0 to CC\n");
#endif
            NvmeWriteReg32(DevExt, NVME_REG_CC, 0);
            if (NvmeWaitForReady(DevExt, FALSE)) {
                break;
            }
        }
    }

    // Step 5: Verify controller is disabled
    csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
    if (csts & NVME_CSTS_RDY) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - FATAL: controller failed to disable after 5 retries, CSTS=%08X\n", csts);
#endif
        return FALSE;
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - controller is now disabled and clean\n");
#endif

    // CRITICAL: QEMU's nvme_ctrl_reset() clears INTMS register to 0 during controller disable!
    // This UNMASKS all interrupts, and if there's stale n->irq_status from the option ROM,
    // QEMU will immediately re-assert the interrupt line.
    // We MUST mask interrupts again after controller reset completes.
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeSanitizeController - re-masking interrupts after controller reset (QEMU clears INTMS during reset)\n");
#endif
    NvmeWriteReg32(DevExt, NVME_REG_INTMS, 0xFFFFFFFF);

    return TRUE;
}

//
// NvmeAllocateQueues - Carve queues, PRP list pool and TRIM pattern out of the
// uncached extension. Done once; (re)initialization reuses them.
//
static BOOLEAN NvmeAllocateQueues(IN PHW_DEVICE_EXTENSION DevExt)
{
    USHORT queueSize = NVME_MAX_QUEUE_SIZE;
    PHYSICAL_ADDRESS trimPhys;
    PVOID trimVirt;

    if (DevExt->ResourcesAllocated) {
        return TRUE;
    }

    // Determine actual queue size - use minimum of our max and controller's max
    if (queueSize > DevExt->MaxQueueEntries) {
        queueSize = DevExt->MaxQueueEntries;
    }

    // Allocate all uncached memory in proper order to avoid alignment waste
    // 1. Admin SQ, 2. I/O SQ (4KB aligned)
    DevExt->AdminQueue.QueueSize = queueSize;
    DevExt->AdminQueue.QueueId = 0;
    DevExt->IoQueue.QueueSize = queueSize;
    DevExt->IoQueue.QueueId = 1;
    if (!AllocateUncachedMemory(DevExt, queueSize * NVME_SQ_ENTRY_SIZE, NVME_PAGE_SIZE,
                                &DevExt->AdminQueue.SubmissionQueue,
                                &DevExt->AdminQueue.SubmissionQueuePhys) ||
        !AllocateUncachedMemory(DevExt, queueSize * NVME_SQ_ENTRY_SIZE, NVME_PAGE_SIZE,
                                &DevExt->IoQueue.SubmissionQueue,
                                &DevExt->IoQueue.SubmissionQueuePhys)) {
        return FALSE;
    }

    // 3. Utility buffer (SgListPages * 4KB)
    // During (re)init: used for Identify commands
    // Otherwise: the PRP list page pool
    if (!AllocateUncachedMemory(DevExt, DevExt->SgListPages << NVME_PAGE_SHIFT, NVME_PAGE_SIZE,
                                &DevExt->UtilityBuffer,
                                &DevExt->UtilityBufferPhys)) {
        return FALSE;
    }
    DevExt->PrpListPages = DevExt->UtilityBuffer;
    DevExt->PrpListPagesPhys = DevExt->UtilityBufferPhys;
    DevExt->PrpListPageBitmap = 0;  // All pages free
    // Every pool page by itself: the uncached extension is not physically
    // contiguous on every system (Windows 9x SCSIPORT.PDR)
    {
        ULONG page;
        for (page = 0; page < DevExt->SgListPages && page < NVMENT_MAX_SG_PAGES; page++) {
            ULONG length = NVME_PAGE_SIZE;
            DevExt->PrpPagePhys[page] = ScsiPortGetPhysicalAddress(DevExt, NULL,
                (PUCHAR)DevExt->PrpListPages + (page << NVME_PAGE_SHIFT), &length);
        }
    }

    // 4./5. Admin CQ and I/O CQ (page-aligned)
    if (!AllocateUncachedMemory(DevExt, queueSize * NVME_CQ_ENTRY_SIZE, NVME_PAGE_SIZE,
                                &DevExt->AdminQueue.CompletionQueue,
                                &DevExt->AdminQueue.CompletionQueuePhys) ||
        !AllocateUncachedMemory(DevExt, queueSize * NVME_CQ_ENTRY_SIZE, NVME_PAGE_SIZE,
                                &DevExt->IoQueue.CompletionQueue,
                                &DevExt->IoQueue.CompletionQueuePhys)) {
        return FALSE;
    }

    // 6. TRIM pattern (optional, kept out of the device extension)
    if (AllocateUncachedMemory(DevExt, NVME_PAGE_SIZE, 4, &trimVirt, &trimPhys)) {
        DevExt->TrimPattern = (PULONG)trimVirt;
    }

    // Calculate log2(QueueSize) and mask for both queues (must be power of 2)
    DevExt->AdminQueue.QueueSizeBits = (UCHAR)log2(DevExt->AdminQueue.QueueSize);
    DevExt->AdminQueue.QueueSizeMask = DevExt->AdminQueue.QueueSize - 1;
    DevExt->IoQueue.QueueSizeBits = (UCHAR)log2(DevExt->IoQueue.QueueSize);
    DevExt->IoQueue.QueueSizeMask = DevExt->IoQueue.QueueSize - 1;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeAllocateQueues - %u entries, uncached memory usage: %u / %u bytes\n",
                   queueSize, DevExt->UncachedExtensionOffset, DevExt->UncachedExtensionSize);
#endif
    DevExt->ResourcesAllocated = TRUE;
    return TRUE;
}

//
// NvmeStartController - Enable the (disabled) controller, create the I/O
// queue pair and find the namespaces. Polled: interrupts stay masked.
//
BOOLEAN NvmeStartController(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG cc, aqa;
    ULONG timeoutMs;
    NVME_COMMAND cmd;

    // Initialize queue state; start with QueueSize so phase = (QueueSize >> bits) & 1 = 1
    DevExt->AdminQueue.SubmissionQueueHead = 0;
    DevExt->AdminQueue.SubmissionQueueTail = 0;
    DevExt->AdminQueue.CompletionQueueHead = DevExt->AdminQueue.QueueSize;
    DevExt->AdminQueue.CompletionQueueTail = 0;
    DevExt->IoQueue.SubmissionQueueHead = 0;
    DevExt->IoQueue.SubmissionQueueTail = 0;
    DevExt->IoQueue.CompletionQueueHead = DevExt->IoQueue.QueueSize;
    DevExt->IoQueue.CompletionQueueTail = 0;

    // Zero out queues: stale phase bits from an earlier run must not look new
    memset(DevExt->AdminQueue.SubmissionQueue, 0, DevExt->AdminQueue.QueueSize * NVME_SQ_ENTRY_SIZE);
    memset(DevExt->AdminQueue.CompletionQueue, 0, DevExt->AdminQueue.QueueSize * NVME_CQ_ENTRY_SIZE);
    memset(DevExt->IoQueue.SubmissionQueue, 0, DevExt->IoQueue.QueueSize * NVME_SQ_ENTRY_SIZE);
    memset(DevExt->IoQueue.CompletionQueue, 0, DevExt->IoQueue.QueueSize * NVME_CQ_ENTRY_SIZE);

    DevExt->PrpListPageBitmap = 0;
    DevExt->CurrentPrpListPagesUsed = 0;
    DevExt->CurrentQueueDepth = 0;
    DevExt->Busy = FALSE;
    DevExt->InitComplete = FALSE;

    // Configure Admin Queue Attributes and addresses
    aqa = ((DevExt->AdminQueue.QueueSize - 1) << 16) | (DevExt->AdminQueue.QueueSize - 1);
    NvmeWriteReg32(DevExt, NVME_REG_AQA, aqa);
    NvmeWriteReg64(DevExt, NVME_REG_ASQ, DevExt->AdminQueue.SubmissionQueuePhys.QuadPart);
    NvmeWriteReg64(DevExt, NVME_REG_ACQ, DevExt->AdminQueue.CompletionQueuePhys.QuadPart);

    cc = NVME_CC_ENABLE |
         ((NVME_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT) |
         NVME_CC_CSS_NVM |
         NVME_CC_AMS_RR |
         NVME_CC_SHN_NONE |
         NVME_CC_IOSQES |
         NVME_CC_IOCQES;

    NvmeWriteReg32(DevExt, NVME_REG_CC, cc);

    // Wait for controller to become ready: CAP.TO in 500 ms units, at least 5 s
    timeoutMs = (ULONG)((DevExt->ControllerCapabilities >> NVME_CAP_TO_SHIFT) & 0xFF) * 500;
    if (timeoutMs < 5000) {
        timeoutMs = 5000;
    }
    while (!(NvmeReadReg32(DevExt, NVME_REG_CSTS) & NVME_CSTS_RDY)) {
        if (timeoutMs-- == 0 || (NvmeReadReg32(DevExt, NVME_REG_CSTS) & NVME_CSTS_CFS)) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NvmeStartController - controller failed to become ready, CSTS=%08X\n",
                           NvmeReadReg32(DevExt, NVME_REG_CSTS));
#endif
            return FALSE;
        }
        ScsiPortStallExecution(1000);
    }

    // Interrupts stay masked (INTMS) while we poll; HwInitialize/NvmeReinitialize unmask
    NvmeWriteReg32(DevExt, NVME_REG_INTMS, 0xFFFFFFFF);

    if (!NvmeCreateIoCQ(DevExt) || !NvmeCreateIoSQ(DevExt) ||
        !NvmeIdentifyController(DevExt) || !NvmeIdentifyNamespace(DevExt)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeStartController - init sequence failed\n");
#endif
        return FALSE;
    }

    // Volatile write cache state, for MODE SENSE
    DevExt->VwcEnabled = FALSE;
    if (DevExt->VwcPresent) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_GET_FEATURES;
        cmd.CDW10 = NVME_FEAT_VOLATILE_WC;
        DevExt->VwcEnabled = TRUE;
        if (NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS)) {
            DevExt->VwcEnabled = (BOOLEAN)((DevExt->SyncDw0 & 1) != 0);
        }
    }

    // Full performance: power state 0. A saved feature value or an earlier
    // driver may have left the controller in a lower (slower) state.
    DevExt->PowerState = 0;
    if (DevExt->Npss != 0) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_GET_FEATURES;
        cmd.CDW10 = NVME_FEAT_POWER_MGMT;
        if (NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS) && (DevExt->SyncDw0 & 0x1F) != 0) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: power state %u found, setting 0\n", DevExt->SyncDw0 & 0x1F);
#endif
            NvmeSetPowerState(DevExt, 0);
        }
    }

    // Autonomous power state transitions with the table built from the
    // power state descriptors (off with APST=0)
    DevExt->ApstOn = FALSE;
    if (DevExt->ApstAllowed && DevExt->ApstSupported && !DevExt->DumpMode) {
        memset(DevExt->UtilityBuffer, 0, NVME_PAGE_SIZE);
        memcpy(DevExt->UtilityBuffer, DevExt->ApstTable, sizeof(DevExt->ApstTable));
        memset(&cmd, 0, sizeof(cmd));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
        cmd.PRP1 = DevExt->UtilityBufferPhys.QuadPart;
        cmd.CDW10 = NVME_FEAT_APST;
        cmd.CDW11 = 1;
        DevExt->ApstOn = NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: APST %s\n", DevExt->ApstOn ? "on" : "refused");
#endif
    }

    // Host memory buffer given before (a reset or power loss took it back)
    DevExt->HmbOn = FALSE;
    if (DevExt->HmbEntries != 0 && !DevExt->DumpMode) NvmeEnableHmb(DevExt);

    // Identify data is in the utility buffer, which is the PRP pool from now on
    memset(DevExt->UtilityBuffer, 0, NVME_PAGE_SIZE);
    DevExt->PrpListPageBitmap = 0;
    DevExt->Stopped = FALSE;
    DevExt->InitComplete = TRUE;

    // Asynchronous events: health warnings, namespace changes, firmware
    // activation; the controller tells instead of being asked
    NvmeStartAer(DevExt);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Init complete - %u LUN(s), VWC %s\n",
                   DevExt->LunCount, DevExt->VwcEnabled ? "on" : "off");
#endif
    return TRUE;
}

//
// NvmeInitializeController - Initialize the NVMe controller with queues and perform device discovery
//
BOOLEAN NvmeInitializeController(IN PHW_DEVICE_EXTENSION DevExt)
{
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeInitializeController called\n");
#endif

    // Read controller capabilities to determine queue sizes and other parameters
    DevExt->ControllerCapabilities = NvmeReadReg64(DevExt, NVME_REG_CAP);
    DevExt->Version = NvmeReadReg32(DevExt, NVME_REG_VS);

    // Parse MQES (Maximum Queue Entries Supported) from CAP register
    // MQES is 0-based, so actual max queue size is MQES + 1
    DevExt->MaxQueueEntries = (USHORT)((DevExt->ControllerCapabilities & NVME_CAP_MQES_MASK) + 1);

    // Calculate doorbell stride (in bytes)
    DevExt->DoorbellStride = 4 << (((ULONG)(DevExt->ControllerCapabilities >> 32) & 0xF));

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeInitializeController - CAP=%08X%08X VS=%08X MQES=%u DBS=%u\n",
                   (ULONG)(DevExt->ControllerCapabilities >> 32),
                   (ULONG)(DevExt->ControllerCapabilities & 0xFFFFFFFF),
                   DevExt->Version, DevExt->MaxQueueEntries, DevExt->DoorbellStride);
#endif

    if (!NvmeAllocateQueues(DevExt)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: NvmeInitializeController - out of uncached memory\n");
#endif
        return FALSE;
    }

    DevExt->NextNonTaggedId = 0;

    // Initialize statistics
    DevExt->MaxQueueDepthReached = 0;
    DevExt->MaxPrpListPagesUsed = 0;
    DevExt->TotalRequests = 0;
    DevExt->TotalReads = 0;
    DevExt->TotalWrites = 0;
    DevExt->TotalBytesRead = 0;
    DevExt->TotalBytesWritten = 0;
    DevExt->MaxReadSize = 0;
    DevExt->MaxWriteSize = 0;
    DevExt->RejectedRequests = 0;

    DevExt->SMARTEnabled = TRUE;

    return NvmeStartController(DevExt);
}

//
// NvmeReinitialize - Set the controller up again after a shutdown, a reset,
// a power transition or anything else that lost its state. Requests still
// outstanding are returned with SRB_STATUS_BUS_RESET; ScsiPort retries them.
//
BOOLEAN NvmeReinitialize(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG l;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeReinitialize (stopped=%u depth=%u)\n",
                   DevExt->Stopped, DevExt->CurrentQueueDepth);
#endif

    // Anything in flight is lost with the controller state. This completes
    // the request HwStartIo is working on as well (RequestsFlushed).
    DevExt->RequestsFlushed = (BOOLEAN)(DevExt->CurrentQueueDepth != 0);
    for (l = 0; l < NVMENT_MAX_LUNS; l++) {
        if (DevExt->Luns[l].NonTaggedInFlight != NULL) {
            DevExt->RequestsFlushed = TRUE;
        }
        DevExt->Luns[l].NonTaggedInFlight = NULL;
    }
    if (DevExt->RequestsFlushed) {
        ScsiPortCompleteRequest(DevExt, 0, SP_UNTAGGED, SP_UNTAGGED, SRB_STATUS_BUS_RESET);
    }
    DevExt->CurrentQueueDepth = 0;
    DevExt->InitComplete = FALSE;

    if (!NvmeSanitizeController(DevExt) || !NvmeStartController(DevExt)) {
        DevExt->Stopped = TRUE;
        return FALSE;
    }
    if (!DevExt->PollMode) {
        NvmeEnableInterrupts(DevExt);
    }
    NvmeArmHotPlug(DevExt);
    ScsiPortNotification(NextRequest, DevExt, NULL);
    return TRUE;
}

//
// NvmeCheckState - Before a request: set the controller up again when it was
// shut down (power down) or lost its state (reset by a sleep state, the BIOS
// or another driver: no longer ready, or fatal status).
//
BOOLEAN NvmeCheckState(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG csts;

    if (DevExt->ControllerRegisters == NULL) {
        return FALSE;
    }
    csts = NvmeReadReg32(DevExt, NVME_REG_CSTS);
    if (csts == 0xFFFFFFFF) {
        return FALSE;                           // gone or powered off: not set up again
    }
    if (!DevExt->Stopped) {
        if ((csts & NVME_CSTS_RDY) && !(csts & NVME_CSTS_CFS)) {
            return TRUE;
        }
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: controller state lost (CSTS=%08X)\n", csts);
#endif
    }
    return NvmeReinitialize(DevExt);
}

//
// NvmeSetPowerState - Set Features Power Management (polled)
//
BOOLEAN NvmeSetPowerState(IN PHW_DEVICE_EXTENSION DevExt, IN UCHAR PowerState)
{
    NVME_COMMAND cmd;

    if (PowerState > DevExt->Npss) {
        return FALSE;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
    cmd.CDW10 = NVME_FEAT_POWER_MGMT;
    cmd.CDW11 = PowerState;
    if (!NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS)) {
        return FALSE;
    }
    DevExt->PowerState = PowerState;
    return TRUE;
}

//
// NvmeFullPower - before a shutdown notification or a power transition:
// autonomous transitions off and power state 0, so the controller takes the
// shutdown at full power, as it did before APST was the default
//
VOID NvmeFullPower(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;

    if (!DevExt->InitComplete || NvmeReadReg32(DevExt, NVME_REG_CSTS) == 0xFFFFFFFF) return;
    if (DevExt->ApstOn) {
        memset(DevExt->UtilityBuffer, 0, NVME_PAGE_SIZE);
        memset(&cmd, 0, sizeof(cmd));
        cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
        cmd.PRP1 = DevExt->UtilityBufferPhys.QuadPart;
        cmd.CDW10 = NVME_FEAT_APST;
        cmd.CDW11 = 0;
        if (NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS)) DevExt->ApstOn = FALSE;
    }
    if (DevExt->Npss != 0 && DevExt->PowerState != 0) NvmeSetPowerState(DevExt, 0);
}

#ifndef NVMENT_W9X
//
// Host memory buffer: memory of the host the controller uses for its own
// tables (DRAM-less SSDs). Taken once, at HwFindAdapter (passive level),
// from the system's contiguous memory, and kept: after a reset or a power
// transition the same buffer is given again (as new: no "memory return").
// Not on 9x and not in the dump stack.
//
#ifdef _WIN64
typedef ULONGLONG NVME_SIZE;
#else
typedef ULONG NVME_SIZE;
#endif
PVOID __stdcall MmAllocateContiguousMemory(NVME_SIZE NumberOfBytes, ULONGLONG HighestAcceptableAddress);
VOID __stdcall MmFreeContiguousMemory(PVOID BaseAddress);
ULONGLONG __stdcall MmGetPhysicalAddress(PVOID BaseAddress);
#if (_WIN32_WINNT >= 0x500)
LONG __stdcall ZwQuerySystemInformation(ULONG InfoClass, PVOID Info, ULONG Length, PULONG ReturnLength);
#else
ULONG __stdcall MmQuerySystemSize(VOID);
#endif

// Upper limit in 4 KB pages: HMB=n MB, else NVMENT_HMB_MAX_MB, and at most
// 1/16 of the RAM (NT 3.1-4.0, which cannot tell the size: 16 MB on a
// large system, none on a small or medium one)
static ULONG NvmeHmbLimit(IN PHW_DEVICE_EXTENSION DevExt)
{
    ULONG limit = (DevExt->HmbCapMb != 0 ? DevExt->HmbCapMb : NVMENT_HMB_MAX_MB) * 256;
#if (_WIN32_WINNT >= 0x500)
    ULONG info[16], len = 0, ram;

    memset(info, 0, sizeof(info));
    // SystemBasicInformation; its exact size is required (44 bytes, 64 on x64)
    if (ZwQuerySystemInformation(0, info, sizeof(NVME_SIZE) == 8 ? 64 : 44, &len) >= 0 && info[2] == 0x1000) {
        ram = info[3];                              // NumberOfPhysicalPages
        if (limit > ram / 16) limit = ram / 16;
    }
#else
    if (MmQuerySystemSize() < 2) {
        limit = 0;
    } else if (DevExt->HmbCapMb == 0 && limit > 16 * 256) {
        limit = 16 * 256;
    }
#endif
    return limit;
}

static VOID NvmeFreeHmb(IN PHW_DEVICE_EXTENSION DevExt, IN PVOID *Virt, IN ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++) MmFreeContiguousMemory(Virt[i]);
    DevExt->HmbEntries = 0;
    DevExt->HmbPages = 0;
}

VOID NvmeSetupHmb(IN PHW_DEVICE_EXTENSION DevExt)
{
    static PVOID virt[NVMENT_HMB_MAX_ENTRIES];
    ULONG want, limit, maxEntries, minChunk, chunk, n = 0, total = 0;
    PULONG list;
    PVOID p;
    ULONGLONG phys;

    if (!DevExt->HmbAllowed || DevExt->DumpMode || DevExt->HmbEntries != 0 || DevExt->Hmpre == 0) return;
    limit = NvmeHmbLimit(DevExt);
    want = DevExt->Hmpre;
    if (want > limit) want = limit;
    if (want == 0 || want < DevExt->Hmmin) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HMB: %u KB preferred, %u KB minimum, limit %u KB: not given\n",
                       DevExt->Hmpre * 4, DevExt->Hmmin * 4, limit * 4);
#endif
        return;
    }
    maxEntries = NVMENT_HMB_MAX_ENTRIES;
    if (DevExt->Hmmaxd != 0 && DevExt->Hmmaxd < maxEntries) maxEntries = DevExt->Hmmaxd;
    minChunk = DevExt->Hmminds != 0 ? DevExt->Hmminds : 1;      // 4 KB units

    list = (PULONG)MmAllocateContiguousMemory(NVME_PAGE_SIZE, ~(ULONGLONG)0);
    if (list == NULL) return;
    memset(list, 0, NVME_PAGE_SIZE);

    chunk = NVMENT_HMB_CHUNK / NVME_PAGE_SIZE;
    while (total < want && n < maxEntries) {
        ULONG size = want - total;
        if (size > chunk) size = chunk;
        if (size < minChunk) size = minChunk;
        p = MmAllocateContiguousMemory((NVME_SIZE)size * NVME_PAGE_SIZE, ~(ULONGLONG)0);
        if (p == NULL) {
            if (chunk / 2 < minChunk || chunk <= 16) break;      // nothing below 64 KB pieces
            chunk /= 2;
            continue;
        }
        phys = MmGetPhysicalAddress(p);
        if (phys & (NVME_PAGE_SIZE - 1)) {                          // never: page-aligned
            MmFreeContiguousMemory(p);
            break;
        }
        virt[n] = p;
        list[n * 4] = (ULONG)phys;
        list[n * 4 + 1] = (ULONG)(phys >> 32);
        list[n * 4 + 2] = size;
        n++;
        total += size;
    }
    DevExt->HmbEntries = (USHORT)n;
    DevExt->HmbPages = total;
    if (total < DevExt->Hmmin || n == 0) {
        NvmeFreeHmb(DevExt, virt, n);
        MmFreeContiguousMemory(list);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HMB: only %u KB of %u KB taken, not given\n", total * 4, want * 4);
#endif
        return;
    }
    DevExt->HmbList = list;
    DevExt->HmbListPhys = MmGetPhysicalAddress(list);
    if (!NvmeEnableHmb(DevExt)) {
        // Refused: the memory goes back to the system
        NvmeFreeHmb(DevExt, virt, n);
        MmFreeContiguousMemory(list);
        DevExt->HmbList = NULL;
    }
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HMB: %u KB in %u piece(s) (preferred %u KB, minimum %u KB): %s\n",
                   total * 4, n, DevExt->Hmpre * 4, DevExt->Hmmin * 4, DevExt->HmbOn ? "on" : "refused");
#endif
}
#else
VOID NvmeSetupHmb(IN PHW_DEVICE_EXTENSION DevExt)
{
    (VOID)DevExt;
}
#endif

//
// NvmeEnableHmb - Set Features Host Memory Buffer with the buffer taken
//
BOOLEAN NvmeEnableHmb(IN PHW_DEVICE_EXTENSION DevExt)
{
    NVME_COMMAND cmd;

    DevExt->HmbOn = FALSE;
    if (DevExt->HmbEntries == 0) return FALSE;
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
    cmd.CDW10 = NVME_FEAT_HMB;
    cmd.CDW11 = 1;                                  // EHM, memory not returned
    cmd.CDW12 = DevExt->HmbPages;                   // HSIZE in memory pages (4 KB)
    cmd.CDW13 = (ULONG)DevExt->HmbListPhys;
    cmd.CDW14 = (ULONG)(DevExt->HmbListPhys >> 32);
    cmd.CDW15 = DevExt->HmbEntries;
    DevExt->HmbOn = NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS);
    return DevExt->HmbOn;
}

//
// NvmeSetVolatileWriteCache - Set Features Volatile Write Cache (polled)
//
BOOLEAN NvmeSetVolatileWriteCache(IN PHW_DEVICE_EXTENSION DevExt, IN BOOLEAN Enable)
{
    NVME_COMMAND cmd;

    if (!DevExt->VwcPresent) {
        return (BOOLEAN)!Enable;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_ADMIN_SET_FEATURES;
    cmd.CDW10 = NVME_FEAT_VOLATILE_WC;
    cmd.CDW11 = Enable ? 1 : 0;
    if (!NvmeAdminSync(DevExt, &cmd, NVMENT_ADMIN_TIMEOUT_MS)) {
        return FALSE;
    }
    DevExt->VwcEnabled = Enable;
    return TRUE;
}

//
// NvmeEnableInterrupts - Enable interrupts at PCI and NVMe controller level
//
VOID NvmeEnableInterrupts(IN PHW_DEVICE_EXTENSION DevExt)
{
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeEnableInterrupts called\n");
#endif

    // Step 1: Enable interrupts at PCI Command Register level (clear bit 10)
    {
        USHORT pciCommand = ReadPciConfigWord(DevExt, PCI_COMMAND_OFFSET);
        pciCommand &= ~PCI_INTERRUPT_DISABLE;  // Clear interrupt disable bit
        WritePciConfigWord(DevExt, PCI_COMMAND_OFFSET, pciCommand);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Enabled interrupts at PCI level, Command=%04X\n", pciCommand);
#endif
    }

    // Step 2: Unmask interrupts at NVMe controller level (unmask vector 0)
    NvmeWriteReg32(DevExt, NVME_REG_INTMC, 0x00000001);
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: Unmasked interrupts at NVMe controller level (INTMC=0x00000001)\n");
#endif
}
