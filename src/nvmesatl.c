// handling SCSI commands
#include "nvment.h"
#include "nvmeutil.h"

static BOOLEAN IsTagged(IN PSCSI_REQUEST_BLOCK Srb)
{
    return (Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE && Srb->QueueTag != SP_UNTAGGED);
}

BOOLEAN ScsiSuccess(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    ScsiPortNotification(RequestComplete, DevExt, Srb);
    ScsiPortNotification(NextRequest, DevExt, NULL);
    return TRUE;
}

BOOLEAN ScsiBusy(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_BUSY;
    ScsiPortNotification(RequestComplete, DevExt, Srb);    
    if (DevExt->CurrentPrpListPagesUsed >= DevExt->SgListPages
        || DevExt->CurrentQueueDepth) {
        DevExt->Busy = TRUE;
    } else {
        ScsiPortNotification(NextRequest, DevExt, NULL);  
    }
    return TRUE;
}

BOOLEAN ScsiError(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR SrbStatus)
{
    Srb->SrbStatus = SrbStatus;
    ScsiPortNotification(RequestComplete, DevExt, Srb);
    ScsiPortNotification(NextRequest, DevExt, NULL);
    return TRUE;
}

BOOLEAN ScsiPending(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN int Next)
{
    Srb->SrbStatus = SRB_STATUS_PENDING;
    if (!Next) {
        DevExt->Busy = TRUE; // completion will send next reqest
        return TRUE;
    }
    if (IsTagged(Srb))
        ScsiPortNotification(NextLuRequest, DevExt, Srb->PathId, Srb->TargetId, Srb->Lun);
    else
        ScsiPortNotification(NextRequest, DevExt, NULL);
    return TRUE;
}

//
// ScsiSetSense - CHECK CONDITION with fixed-format sense data: auto-sense
// where the request allows it, otherwise kept for the REQUEST SENSE that
// follows (ScsiPort disables auto-sense for some pass-through requests)
//
VOID ScsiSetSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR SenseKey, IN UCHAR Asc, IN UCHAR Ascq)
{
    UCHAR sense[18];
    ULONG n;

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

    memset(sense, 0, sizeof(sense));
    sense[0] = 0x70;                // current error, fixed format
    sense[2] = SenseKey;
    sense[7] = 10;
    sense[12] = Asc;
    sense[13] = Ascq;

    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength >= 14 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        n = Srb->SenseInfoBufferLength;
        memset(Srb->SenseInfoBuffer, 0, n);
        if (n > sizeof(sense)) n = sizeof(sense);
        if (n < sizeof(sense)) sense[7] = (UCHAR)(n - 8);
        memcpy(Srb->SenseInfoBuffer, sense, n);
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
        return;
    }
    if (NVME_LUN_INDEX(Srb) < NVMENT_MAX_LUNS) {
        memcpy(DevExt->Luns[NVME_LUN_INDEX(Srb)].Sense, sense, sizeof(sense));
        DevExt->Luns[NVME_LUN_INDEX(Srb)].SensePending = TRUE;
    }
}

//
// ScsiHandleRequestSense - the sense of the last failed request (no
// auto-sense), otherwise NO SENSE
//
BOOLEAN ScsiHandleRequestSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    UCHAR sense[18];
    ULONG n = Srb->Cdb[4];

    if (lun->SensePending) {
        memcpy(sense, lun->Sense, sizeof(sense));
        lun->SensePending = FALSE;
    } else {
        memset(sense, 0, sizeof(sense));
        sense[0] = 0x70;
        sense[7] = 10;
    }
    if (n > sizeof(sense)) n = sizeof(sense);
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    if (n != 0 && Srb->DataBuffer != NULL) memcpy(Srb->DataBuffer, sense, n);
    Srb->DataTransferLength = n;
    return ScsiSuccess(DevExt, Srb);
}

//
// NvmeSetSenseFromStatus - Translate an NVMe completion status (Status >> 1:
// SC in bits 7:0, SCT in bits 10:8, DNR in bit 14) into SCSI sense data,
// along the lines of the NVM Express SCSI Translation Reference
//
VOID NvmeSetSenseFromStatus(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN USHORT Status)
{
    UCHAR sc = NVME_STATUS_SC(Status);
    UCHAR key = SCSI_SENSE_HARDWARE_ERROR;
    UCHAR asc = ASC_INTERNAL_TARGET_FAILURE;
    UCHAR ascq = 0;

    switch (NVME_STATUS_SCT(Status)) {
        case NVME_SCT_GENERIC:
            switch (sc) {
                case NVME_SC_INVALID_OPCODE:
                    key = SCSI_SENSE_ILLEGAL_REQUEST; asc = ASC_INVALID_COMMAND_OPCODE; break;
                case NVME_SC_INVALID_FIELD:
                    key = SCSI_SENSE_ILLEGAL_REQUEST; asc = ASC_INVALID_FIELD_IN_CDB; break;
                case NVME_SC_DATA_XFER_ERROR:
                case NVME_SC_CAP_EXCEEDED:
                    key = SCSI_SENSE_MEDIUM_ERROR; asc = ASC_NO_ADDITIONAL_SENSE; break;
                case NVME_SC_POWER_LOSS:
                case NVME_SC_ABORT_REQ:
                case NVME_SC_ABORT_QUEUE:
                case NVME_SC_FUSED_FAIL:
                case NVME_SC_FUSED_MISSING:
                    key = SCSI_SENSE_ABORTED_COMMAND; asc = ASC_NO_ADDITIONAL_SENSE; break;
                case NVME_SC_INVALID_NS:
                    key = SCSI_SENSE_ILLEGAL_REQUEST; asc = ASC_LUN_NOT_SUPPORTED; break;
                case NVME_SC_LBA_RANGE:
                    key = SCSI_SENSE_ILLEGAL_REQUEST; asc = ASC_LBA_OUT_OF_RANGE; break;
                case NVME_SC_NS_NOT_READY:
                    key = SCSI_SENSE_NOT_READY; asc = ASC_LUN_NOT_READY; break;
                case NVME_SC_RESERVATION_CONFLICT:
                    Srb->SrbStatus = SRB_STATUS_ERROR;
                    Srb->ScsiStatus = SCSISTAT_RESERVATION_CONFLICT;
                    return;
                default:
                    break;
            }
            break;

        case NVME_SCT_COMMAND_SPECIFIC:
            key = SCSI_SENSE_ILLEGAL_REQUEST; asc = ASC_INVALID_FIELD_IN_CDB;
            break;

        case NVME_SCT_MEDIA:
            key = SCSI_SENSE_MEDIUM_ERROR;
            switch (sc) {
                case NVME_SC_WRITE_FAULT:     asc = ASC_WRITE_ERROR; break;
                case NVME_SC_READ_ERROR:      asc = ASC_UNRECOVERED_READ_ERROR; break;
                case NVME_SC_GUARD_CHECK:     asc = ASC_ID_CRC_ERROR; ascq = 1; break;
                case NVME_SC_APPTAG_CHECK:    asc = ASC_ID_CRC_ERROR; ascq = 2; break;
                case NVME_SC_REFTAG_CHECK:    asc = ASC_ID_CRC_ERROR; ascq = 3; break;
                case NVME_SC_COMPARE_FAILED:  key = SCSI_SENSE_MISCOMPARE; asc = ASC_MISCOMPARE; break;
                case NVME_SC_ACCESS_DENIED:   key = SCSI_SENSE_DATA_PROTECT; asc = ASC_WRITE_PROTECTED; break;
                default:                      asc = ASC_NO_ADDITIONAL_SENSE; break;
            }
            break;

        default:
            break;
    }
    ScsiSetSense(DevExt, Srb, key, asc, ascq);
}

//
// ScsiGetLbaCount - LBA, block count, direction and FUA of a READ/WRITE/VERIFY CDB
//
BOOLEAN ScsiGetLbaCount(IN PSCSI_REQUEST_BLOCK Srb, OUT PULONGLONG Lba, OUT PULONG Count, OUT PBOOLEAN IsWrite, OUT PBOOLEAN Fua)
{
    PUCHAR cdb = Srb->Cdb;

    *Lba = 0;
    *Count = 0;
    *IsWrite = FALSE;
    *Fua = FALSE;

    switch (cdb[0]) {
        case SCSIOP_READ6:
        case SCSIOP_WRITE6:
            *Lba = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
            *Count = cdb[4] ? cdb[4] : 256;  // 0 means 256 blocks in READ(6)/WRITE(6)
            *IsWrite = (BOOLEAN)(cdb[0] == SCSIOP_WRITE6);
            return TRUE;

        case SCSIOP_READ:
        case SCSIOP_WRITE:
        case SCSIOP_VERIFY:
        case SCSIOP_WRITE_VERIFY:
            *Lba = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
            *Count = ((ULONG)cdb[7] << 8) | cdb[8];
            break;

        case SCSIOP_READ12:
        case SCSIOP_WRITE12:
        case SCSIOP_VERIFY12:
            *Lba = ((ULONG)cdb[2] << 24) | ((ULONG)cdb[3] << 16) | ((ULONG)cdb[4] << 8) | cdb[5];
            *Count = ((ULONG)cdb[6] << 24) | ((ULONG)cdb[7] << 16) | ((ULONG)cdb[8] << 8) | cdb[9];
            break;

        case SCSIOP_READ16:
        case SCSIOP_WRITE16:
        case SCSIOP_VERIFY16:
            *Lba = ((ULONGLONG)cdb[2] << 56) | ((ULONGLONG)cdb[3] << 48) |
                   ((ULONGLONG)cdb[4] << 40) | ((ULONGLONG)cdb[5] << 32) |
                   ((ULONGLONG)cdb[6] << 24) | ((ULONGLONG)cdb[7] << 16) |
                   ((ULONGLONG)cdb[8] << 8) | ((ULONGLONG)cdb[9]);
            *Count = ((ULONG)cdb[10] << 24) | ((ULONG)cdb[11] << 16) | ((ULONG)cdb[12] << 8) | cdb[13];
            break;

        default:
            return FALSE;
    }

    *IsWrite = (BOOLEAN)(cdb[0] == SCSIOP_WRITE || cdb[0] == SCSIOP_WRITE12 ||
                         cdb[0] == SCSIOP_WRITE16 || cdb[0] == SCSIOP_WRITE_VERIFY);
    *Fua = (BOOLEAN)((cdb[1] & 0x08) != 0);  // FUA bit of the 10/12/16-byte CDBs
    return TRUE;
}

//
// ScsiVpdDone - return a VPD page built in a local buffer, truncated to the allocation length
//
static BOOLEAN ScsiVpdDone(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PUCHAR Page, IN ULONG Length)
{
    if (Length > Srb->DataTransferLength) {
        Length = Srb->DataTransferLength;
    }
    memcpy(Srb->DataBuffer, Page, Length);
    Srb->DataTransferLength = Length;
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleInquiry - Handle SCSI INQUIRY command
//
BOOLEAN ScsiHandleInquiry(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PCDB cdb = (PCDB)Srb->Cdb;
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    BOOLEAN dsm = (BOOLEAN)((DevExt->Oncs & NVME_ONCS_DSM) != 0);
    PUCHAR inquiryData;
    UCHAR vpd[64];
    ULONG i, j;

    if (Srb->DataTransferLength < 5) {
        return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
    }

    inquiryData = (PUCHAR)Srb->DataBuffer;
    memset(inquiryData, 0, Srb->DataTransferLength);

    // EVPD is bit 0 of CDB byte 1, the page code is CDB byte 2
    if (Srb->Cdb[1] & 0x01) {
        // VPD page requested
        UCHAR pageCode = Srb->Cdb[2];

        // build the page locally, the caller may ask for less than all of it
        inquiryData = vpd;
        memset(vpd, 0, sizeof(vpd));

        if (pageCode == 0x00) {
            // VPD page 0x00: Supported VPD Pages
            inquiryData[0] = 0x00;  // Peripheral Device Type: Direct access
            inquiryData[1] = 0x00;  // Page Code 0x00
            inquiryData[2] = 0x00;  // Reserved
            inquiryData[3] = 0x04;  // Page Length (4 pages supported)
            inquiryData[4] = 0x00;  // Supported page: 0x00 (this page)
            inquiryData[5] = 0x80;  // Supported page: 0x80 (Unit Serial Number)
            inquiryData[6] = 0xB0;  // Supported page: 0xB0 (Block Limits)
            inquiryData[7] = 0xB1;  // Supported page: 0xB1 (Block Device Characteristics)
            if (dsm) {
                inquiryData[3] = 0x05;
                inquiryData[8] = 0xB2;  // Supported page: 0xB2 (Logical Block Provisioning)
            }

            return ScsiVpdDone(DevExt, Srb, inquiryData, 4 + inquiryData[3]);
        } else if (pageCode == 0x80) {
            // VPD page 0x80: Unit Serial Number (SPC-3)
            UCHAR serialLength = 20;  // NVMe serial numbers are 20 bytes
            ULONG pageLength = 4 + serialLength;  // Header (4 bytes) + serial number

            inquiryData[0] = 0x00;  // Peripheral Device Type: Direct access
            inquiryData[1] = 0x80;  // Page Code: Unit Serial Number
            inquiryData[2] = 0x00;  // Reserved
            inquiryData[3] = serialLength;  // Page Length (20 bytes)

            // Copy serial number from controller (trim trailing spaces)
            memcpy(&inquiryData[4], DevExt->ControllerSerialNumber, serialLength);

            return ScsiVpdDone(DevExt, Srb, inquiryData, pageLength);
        } else if (pageCode == 0xB0) {
            // VPD page 0xB0: Block Limits (SBC-3)
            ULONG maxTransferBlocks;

            // Calculate maximum transfer length in blocks
            if (lun->BlockSize > 0) {
                maxTransferBlocks = DevExt->MaxTransferSizeBytes >> lun->BlockShift;
            } else {
                maxTransferBlocks = DevExt->MaxTransferSizeBytes / 512;  // Assume 512 if not initialized
            }

            inquiryData[0] = 0x00;  // Peripheral Device Type: Direct access
            inquiryData[1] = 0xB0;  // Page Code: Block Limits
            inquiryData[2] = 0x00;  // Page Length (MSB)
            inquiryData[3] = 0x3C;  // Page Length (LSB) - 60 bytes

            // Byte 4: WSNZ (Write Same No Zero) - 0
            inquiryData[4] = 0x00;

            // Byte 5: Maximum Compare and Write Length - 0 (not supported)
            inquiryData[5] = 0x00;

            // Bytes 6-7: Optimal Transfer Length Granularity - 0
            inquiryData[6] = 0x00;
            inquiryData[7] = 0x00;

            // Bytes 8-11: Maximum Transfer Length (in blocks)
            inquiryData[8] = (UCHAR)((maxTransferBlocks >> 24) & 0xFF);
            inquiryData[9] = (UCHAR)((maxTransferBlocks >> 16) & 0xFF);
            inquiryData[10] = (UCHAR)((maxTransferBlocks >> 8) & 0xFF);
            inquiryData[11] = (UCHAR)(maxTransferBlocks & 0xFF);

            // Bytes 12-15: Optimal Transfer Length - same as maximum
            inquiryData[12] = inquiryData[8];
            inquiryData[13] = inquiryData[9];
            inquiryData[14] = inquiryData[10];
            inquiryData[15] = inquiryData[11];

            // Bytes 16-19: Maximum Prefetch/XDRead/XDWrite Transfer Length - 0
            inquiryData[16] = 0x00;
            inquiryData[17] = 0x00;
            inquiryData[18] = 0x00;
            inquiryData[19] = 0x00;

            // Bytes 20-31: UNMAP limits, only with Dataset Management (UNMAP -> DSM deallocate)
            // Maximum Unmap LBA Count 0xFFFFFFFF, Maximum Unmap Block Descriptor Count 256
            // (one DSM command, one page of ranges), Optimal Unmap Granularity 1
            if (dsm) {
                inquiryData[20] = 0xFF;
                inquiryData[21] = 0xFF;
                inquiryData[22] = 0xFF;
                inquiryData[23] = 0xFF;
                inquiryData[26] = 0x01;
                inquiryData[27] = 0x00;
                inquiryData[31] = 0x01;
            }

            // Bytes 32-35: Unmap Granularity Alignment - 0
            inquiryData[32] = 0x00;
            inquiryData[33] = 0x00;
            inquiryData[34] = 0x00;
            inquiryData[35] = 0x00;

            // Bytes 36-43: Maximum Write Same Length - 0 (not supported)
            inquiryData[36] = 0x00;
            inquiryData[37] = 0x00;
            inquiryData[38] = 0x00;
            inquiryData[39] = 0x00;
            inquiryData[40] = 0x00;
            inquiryData[41] = 0x00;
            inquiryData[42] = 0x00;
            inquiryData[43] = 0x00;

            // Bytes 44-47: Maximum Atomic Transfer Length - 0
            inquiryData[44] = 0x00;
            inquiryData[45] = 0x00;
            inquiryData[46] = 0x00;
            inquiryData[47] = 0x00;

            // Bytes 48-51: Atomic Alignment - 0
            inquiryData[48] = 0x00;
            inquiryData[49] = 0x00;
            inquiryData[50] = 0x00;
            inquiryData[51] = 0x00;

            // Bytes 52-55: Atomic Transfer Length Granularity - 0
            inquiryData[52] = 0x00;
            inquiryData[53] = 0x00;
            inquiryData[54] = 0x00;
            inquiryData[55] = 0x00;

            // Bytes 56-59: Maximum Atomic Transfer Length With Atomic Boundary - 0
            inquiryData[56] = 0x00;
            inquiryData[57] = 0x00;
            inquiryData[58] = 0x00;
            inquiryData[59] = 0x00;

            // Bytes 60-63: Maximum Atomic Boundary Size - 0
            inquiryData[60] = 0x00;
            inquiryData[61] = 0x00;
            inquiryData[62] = 0x00;
            inquiryData[63] = 0x00;

            return ScsiVpdDone(DevExt, Srb, inquiryData, 64);
        } else if (pageCode == 0xB1) {
            // VPD page 0xB1: Block Device Characteristics (SBC-3)
            inquiryData[0] = 0x00;  // Peripheral Device Type: Direct access
            inquiryData[1] = 0xB1;  // Page Code: Block Device Characteristics
            inquiryData[2] = 0x00;  // Page Length (MSB)
            inquiryData[3] = 0x3C;  // Page Length (LSB) - 60 bytes

            // Bytes 4-5: Medium rotation rate (big-endian)
            // 0x0001 = Non-rotating medium (SSD)
            inquiryData[4] = 0x00;
            inquiryData[5] = 0x01;

            // Byte 6: Product type (0 = not indicated)
            inquiryData[6] = 0x00;

            // Byte 7: WABEREQ, WACEREQ, Nominal form factor, VBULS
            // Bits 7-6: WABEREQ (Write After Block Erase Required) = 00b (not required)
            // Bits 5-4: WACEREQ (Write After Cryptographic Erase Required) = 00b (not required)
            // Bits 3-0: Nominal form factor = Ch (M.2 22110)
            inquiryData[7] = 0x0C;

            // Bytes 8-9: DEPOPULATION TIME (big-endian, in seconds)
            // 0 = not reported
            inquiryData[8] = 0x00;
            inquiryData[9] = 0x00;

            // Bytes 10-63: Reserved
            memset(&inquiryData[10], 0, 54);

            return ScsiVpdDone(DevExt, Srb, inquiryData, 64);
        } else if (pageCode == 0xB2 && dsm) {
            // VPD page 0xB2: Logical Block Provisioning (SBC-3)
            inquiryData[1] = 0xB2;
            inquiryData[3] = 0x04;
            inquiryData[5] = 0x80;                   // LBPU: UNMAP supported
            inquiryData[6] = lun->Thin ? 0x02 : 0x00; // provisioning type: thin / not reported
            return ScsiVpdDone(DevExt, Srb, inquiryData, 8);
        } else {
            // Unsupported VPD page
            ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0);
            return ScsiError(DevExt, Srb, Srb->SrbStatus);
        }
    }

    // Standard INQUIRY data
    if (Srb->DataTransferLength >= 36) {
        inquiryData[0] = 0x00;  // Peripheral Device Type: Direct access block device
        inquiryData[1] = 0x00;  // RMB = 0 (not removable)
        inquiryData[2] = 0x05;  // Version: SPC-3
        inquiryData[3] = 0x02;  // Response Data Format
        inquiryData[4] = 0x1F;  // Additional Length (31 bytes following)
        inquiryData[5] = 0x00;  // SCCS = 0
        inquiryData[6] = 0x00;  // No special features
        inquiryData[7] = 0x02;  // CmdQue = 1 (supports tagged command queuing)

        // Vendor Identification (8 bytes) - Extract first 8 chars from model number
        // Skip leading spaces in model number
        i = 0;
        while (i < 40 && DevExt->ControllerModelNumber[i] == ' ') {
            i++;
        }

        // Copy up to 8 characters for vendor field
        for (j = 0; j < 8 && i < 40 && DevExt->ControllerModelNumber[i] != 0; j++, i++) {
            inquiryData[8 + j] = DevExt->ControllerModelNumber[i];
        }
        // Pad with spaces if needed
        while (j < 8) {
            inquiryData[8 + j] = ' ';
            j++;
        }

        // Product Identification (16 bytes) - Continue from model number
        for (j = 0; j < 16 && i < 40 && DevExt->ControllerModelNumber[i] != 0; j++, i++) {
            inquiryData[16 + j] = DevExt->ControllerModelNumber[i];
        }
        // Pad with spaces if needed
        while (j < 16) {
            inquiryData[16 + j] = ' ';
            j++;
        }

        // Product Revision Level (4 bytes) - Use firmware revision
        for (j = 0; j < 4 && j < 8 && DevExt->ControllerFirmwareRevision[j] != 0; j++) {
            inquiryData[32 + j] = DevExt->ControllerFirmwareRevision[j];
        }
        // Pad with spaces if needed
        while (j < 4) {
            inquiryData[32 + j] = ' ';
            j++;
        }

        Srb->DataTransferLength = 36;
    }

    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleReadCapacity - Handle SCSI READ CAPACITY(10) command
//
BOOLEAN ScsiHandleReadCapacity(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PULONG capacityData;
    ULONG lastLba;
    ULONG blockSize;
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];

    if (Srb->DataTransferLength < 8) {
        return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
    }

    capacityData = (PULONG)Srb->DataBuffer;

    if (lun->SizeInBlocks == 0) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_NOT_READY, 0x04, 0x00);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    {
        // 2 TB and more (last LBA FFFFFFFFh or beyond): FFFFFFFFh sends a class
        // driver that knows READ CAPACITY(16) there. One that does not (NT
        // 3.x/4.0, 9x, 2000, XP x86) adds one to it in 32 bits and sees a disk
        // of 0 bytes; it gets FFFFFFFEh, the most it can address with READ(10).
        if (lun->SizeInBlocks >= 0x100000000ULL) {
            lastLba = DevExt->Capacity16 ? 0xFFFFFFFF : 0xFFFFFFFE;
        } else {
            lastLba = (ULONG)(lun->SizeInBlocks - 1);
        }
        blockSize = lun->BlockSize;
    }

    // Return in big-endian format
    capacityData[0] = ((lastLba & 0xFF) << 24) |
                      ((lastLba & 0xFF00) << 8) |
                      ((lastLba & 0xFF0000) >> 8) |
                      ((lastLba & 0xFF000000) >> 24);

    capacityData[1] = ((blockSize & 0xFF) << 24) |
                      ((blockSize & 0xFF00) << 8) |
                      ((blockSize & 0xFF0000) >> 8) |
                      ((blockSize & 0xFF000000) >> 24);

    Srb->DataTransferLength = 8;
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleReadCapacity16 - Handle SCSI READ CAPACITY(16) command
// Per SBC-3 section 5.12 - Returns 64-bit LBA and additional information
//
BOOLEAN ScsiHandleReadCapacity16(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR capacityData[32];
    ULONGLONG lastLba;
    ULONG blockSize;
    ULONG allocation, n;

    // Check service action (byte 1 of CDB)
    if ((Srb->Cdb[1] & 0x1F) != SERVICE_ACTION_READ_CAPACITY16) {
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }

    // As much as the allocation length and the buffer take: the class
    // drivers of Server 2003 SP1 and XP x64 ask for 12 bytes
    allocation = ((ULONG)Srb->Cdb[10] << 24) | ((ULONG)Srb->Cdb[11] << 16) |
                 ((ULONG)Srb->Cdb[12] << 8) | Srb->Cdb[13];
    n = sizeof(capacityData);
    if (n > allocation) n = allocation;
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    if (n == 0 || Srb->DataBuffer == NULL) {
        return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
    }

    memset(capacityData, 0, sizeof(capacityData));

    if (DevExt->Luns[NVME_LUN_INDEX(Srb)].SizeInBlocks == 0) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_NOT_READY, 0x04, 0x00);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    lastLba = DevExt->Luns[NVME_LUN_INDEX(Srb)].SizeInBlocks - 1;
    blockSize = DevExt->Luns[NVME_LUN_INDEX(Srb)].BlockSize;

    // Bytes 0-7: Returned Logical Block Address (64-bit big-endian)
    capacityData[0] = (UCHAR)(lastLba >> 56);
    capacityData[1] = (UCHAR)(lastLba >> 48);
    capacityData[2] = (UCHAR)(lastLba >> 40);
    capacityData[3] = (UCHAR)(lastLba >> 32);
    capacityData[4] = (UCHAR)(lastLba >> 24);
    capacityData[5] = (UCHAR)(lastLba >> 16);
    capacityData[6] = (UCHAR)(lastLba >> 8);
    capacityData[7] = (UCHAR)(lastLba);

    // Bytes 8-11: Block Length in Bytes (32-bit big-endian)
    capacityData[8] = (UCHAR)(blockSize >> 24);
    capacityData[9] = (UCHAR)(blockSize >> 16);
    capacityData[10] = (UCHAR)(blockSize >> 8);
    capacityData[11] = (UCHAR)(blockSize);

    // Byte 14, bit 7: LBPME - UNMAP is translated to Dataset Management (deallocate)
    if (DevExt->Oncs & NVME_ONCS_DSM) {
        capacityData[14] = 0x80;
    }

    memcpy(Srb->DataBuffer, capacityData, n);
    Srb->DataTransferLength = n;
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleReadWrite - Handle SCSI READ/WRITE commands
//
BOOLEAN ScsiHandleReadWrite(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    NVME_COMMAND nvmeCmd;
    USHORT commandId;
    PNVME_SRB_EXTENSION srbExt;
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    ULONGLONG lba;
    ULONG count;
    BOOLEAN isWrite, fua;
    int rc;

    // A namespace that was not identified has no capacity: not ready,
    // never busy (a busy answer would be retried for ever at boot)
    if (lun->SizeInBlocks == 0) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_NOT_READY, 0x04, 0x00);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }

    // A transfer length of 0 moves nothing (NVMe cannot express 0 blocks)
    ScsiGetLbaCount(Srb, &lba, &count, &isWrite, &fua);
    if (count == 0) {
        Srb->DataTransferLength = 0;
        return ScsiSuccess(DevExt, Srb);
    }
    if (lba >= lun->SizeInBlocks || count > lun->SizeInBlocks - lba) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }

    // Validate transfer size against MDTS
    if (Srb->DataTransferLength > DevExt->MaxTransferSizeBytes) {
        ScsiDebugPrint(0, "NVMENT: Buffer size %u exceeds MDTS limit %u - rejecting\n",
                       Srb->DataTransferLength, DevExt->MaxTransferSizeBytes);
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Buffer size %u exceeds MDTS limit %u - rejecting\n",
                       Srb->DataTransferLength, DevExt->MaxTransferSizeBytes);
#endif
        DevExt->RejectedRequests++;        
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }

    // Check if this is a non-tagged request (QueueTag == SP_UNTAGGED or no queue action enabled)
    if (!((Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) && (Srb->QueueTag != SP_UNTAGGED))) {
        // Non-tagged request - only one can be in flight at a time
        if (lun->NonTaggedInFlight) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Non-tagged request rejected - another non-tagged request in flight tag:%02X\n", Srb->QueueTag);
#endif
            return ScsiBusy(DevExt, Srb);
        }
        // Mark that we now have a non-tagged request in flight
        lun->NonTaggedInFlight = Srb;
    }

#ifdef NVMENT_DBG_EXTRA
    // Log HEAD_OF_QUEUE tags (treated as SIMPLE since NVMe has no priority queuing)
    if ((Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) &&
        (Srb->QueueAction == SRB_HEAD_OF_QUEUE_TAG_REQUEST)) {
        ScsiDebugPrint(0, "NVMENT: HEAD_OF_QUEUE tag - treating as SIMPLE (no HW priority support)\n");
    }
#endif

    // For ORDERED tags, submit a Flush command first to drain all prior operations
    if ((Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) &&
        (Srb->QueueAction == SRB_ORDERED_QUEUE_TAG_REQUEST)) {
        NVME_COMMAND flushCmd;
        USHORT flushCommandId;

#ifdef NVMENT_DBG_EXTRA
        ScsiDebugPrint(0, "NVMENT: ORDERED tag on I/O - submitting Flush before I/O\n");
#endif
        // Build flush command with special ORDERED flush CID
        flushCommandId = NvmeBuildFlushCommandId(Srb);

        memset(&flushCmd, 0, sizeof(NVME_COMMAND));
        flushCmd.CDW0.Fields.Opcode = NVME_CMD_FLUSH;
        flushCmd.CDW0.Fields.Flags = 0;
        flushCmd.CDW0.Fields.CommandId = flushCommandId;
        flushCmd.NSID = lun->NamespaceId;

#ifdef NVMENT_DBG_EXTRA
        ScsiDebugPrint(0, "NVMENT: Submitting Flush (CID=%d) before ORDERED I/O\n", flushCommandId);
#endif
        if (!NvmeSubmitIoCommand(DevExt, &flushCmd)) {
            // Flush submission failed
            return ScsiBusy(DevExt, Srb);
        }
    }

    // Build command ID for the I/O command
    commandId = NvmeBuildCommandId(DevExt, Srb);

    // Initialize SRB extension
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
    srbExt->PrpListPage = 0xFF;  // No PRP list initially

    // Build the NVMe Read/Write command from the SCSI CDB.
    memset(&nvmeCmd, 0, sizeof(NVME_COMMAND));
    rc = NvmeBuildReadWriteCommand(DevExt, Srb, &nvmeCmd, commandId);
    if (rc <=0) {
        // most likely couldnt get memory for PRP list
        
        if (rc == 0) {
            lun->NonTaggedInFlight = NULL;
            return ScsiBusy(DevExt, Srb);
        } else {
            // NonTaggedInFlight cleared by by callee to maintain ordering
            // ScsiError called by callee
            return TRUE; 
        }
    }

    // Submit the command to the I/O queue.
    if (NvmeSubmitIoCommand(DevExt, &nvmeCmd)) {
        // Command submitted successfully, mark SRB as pending.
        return ScsiPending(DevExt, Srb, 
            DevExt->CurrentPrpListPagesUsed < (ULONG)(DevExt->SgListPages)
            || DevExt->CurrentQueueDepth >= NVME_MAX_QUEUE_SIZE);
    } else {
        // Submission failed, likely a full queue. Free resources and mark as busy.
        if (srbExt->PrpListPage != 0xFF) {
            FreePrpListPage(DevExt, srbExt->PrpListPage);
            srbExt->PrpListPage = 0xFF;
        }
        lun->NonTaggedInFlight = NULL;
        return ScsiBusy(DevExt, Srb);
    }
}

//
// ScsiHandleLogSense - Handle SCSI LOG SENSE command
// Translates to NVMe Get Log Page for SMART/Health data
//
BOOLEAN ScsiHandleLogSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR pageCode;

    // Check if the controller is set up
    if (!DevExt->InitComplete) {
        return ScsiBusy(DevExt, Srb);
    }

    // Extract page code from CDB
    pageCode = ScsiGetLogPageCodeFromSrb(Srb);

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: ScsiHandleLogSense - Page Code=0x%02X\n", pageCode);
#endif

    // Supported pages: answered here
    if (pageCode == SCSI_LOG_PAGE_SUPPORTED_PAGES) {
        UCHAR page[8];
        ULONG allocLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], len = 8;
        memset(page, 0, sizeof(page));
        page[3] = 4;
        page[4] = SCSI_LOG_PAGE_SUPPORTED_PAGES;
        page[5] = SCSI_LOG_PAGE_TEMPERATURE;
        page[6] = SCSI_LOG_PAGE_SOLID_STATE;
        page[7] = SCSI_LOG_PAGE_INFORMATIONAL;
        if (len > allocLen) len = allocLen;
        if (len > Srb->DataTransferLength) len = Srb->DataTransferLength;
        memset(Srb->DataBuffer, 0, Srb->DataTransferLength);
        memcpy(Srb->DataBuffer, page, len);
        Srb->DataTransferLength = len;
        return ScsiSuccess(DevExt, Srb);
    }

    // Temperature, solid state media, informational exceptions: from the
    // SMART / Health Information log
    if (pageCode == SCSI_LOG_PAGE_INFORMATIONAL || pageCode == SCSI_LOG_PAGE_TEMPERATURE ||
        pageCode == SCSI_LOG_PAGE_SOLID_STATE) {

        // Issue async NVMe Get Log Page command for SMART/Health info
        if (!NvmeGetLogPage(DevExt, Srb, NVME_LOG_PAGE_SMART_HEALTH)) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Failed to submit Get Log Page for LOG SENSE\n");
#endif        
            return ScsiError(DevExt, Srb, SRB_STATUS_ERROR);
        }

        // Mark SRB as pending - will be completed in interrupt handler
        return ScsiPending(DevExt, Srb, 1);
    } else {
        // Unsupported log page
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }
}

//
// ScsiHandleSatPassthrough - Handle SAT ATA PASS-THROUGH commands (0x85, 0xA1)
// Only supports SMART read operations (SMART READ DATA and IDENTIFY DEVICE)
//
BOOLEAN ScsiHandleSatPassthrough(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR ataCommand;
    UCHAR ataFeatures;
    UCHAR ataCylLow;
    UCHAR ataCylHigh;

    // Check if the controller is set up
    if (!DevExt->InitComplete) {
        return ScsiBusy(DevExt, Srb);
    }

    // Parse and validate the SAT command
    if (!ScsiParseSatCommand(Srb, &ataCommand, &ataFeatures, &ataCylLow, &ataCylHigh)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: SAT command rejected - not a supported SMART read\n");
#endif
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: SAT PASS-THROUGH - ATA Cmd=0x%02X Feature=0x%02X CylLow=0x%02X CylHi=0x%02X\n",
                   ataCommand, ataFeatures, ataCylLow, ataCylHigh);
#endif

    // Check if this is SMART READ DATA or IDENTIFY DEVICE
    if (ataCommand == ATA_SMART_CMD && ataFeatures == ATA_SMART_READ_DATA) {
        // SMART READ DATA - translate to NVMe Get Log Page
        // Ensure we have buffer space for output (512 bytes)
        if (Srb->DataTransferLength < 512) {
            return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
        }

        // Issue async NVMe Get Log Page command for SMART/Health info
        if (!NvmeGetLogPage(DevExt, Srb, NVME_LOG_PAGE_SMART_HEALTH)) {
            return ScsiError(DevExt, Srb, SRB_STATUS_ERROR);
        }

        // Mark SRB as pending - will be completed in interrupt handler
        return ScsiPending(DevExt, Srb, 1);
    } else if (ataCommand == ATA_SMART_CMD && ataFeatures == ATA_SMART_READ_LOG) {
        // SMART READ LOG - return empty log (NVMe doesn't support ATA-style log pages)
        // Ensure we have buffer space for output (512 bytes)
        if (Srb->DataTransferLength < 512) {
            return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
        }

        // Return empty 512-byte buffer (all zeros)
        memset(Srb->DataBuffer, 0, 512);
        Srb->DataTransferLength = 512;

        return ScsiSuccess(DevExt, Srb);    
    } else if (ataCommand == ATA_IDENTIFY_DEVICE) {
        if (Srb->DataTransferLength < 512) {
            return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
        }

        NvmeToAtaIdentify(DevExt, (PATA_IDENTIFY_DEVICE_STRUCT)Srb->DataBuffer);
        return ScsiSuccess(DevExt, Srb);
    } else {
        // Unknown command (should not reach here due to validation)
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }
}

//
// ScsiHandleFlush - Handle SCSI SYNCHRONIZE_CACHE command by sending NVMe Flush
//
BOOLEAN ScsiHandleFlush(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    NVME_COMMAND nvmeCmd;
    USHORT commandId;
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];

    // No namespace on this LUN (SHUTDOWN/FLUSH for the adapter): nothing to flush
    if (lun->NamespaceId == 0) {
        return ScsiSuccess(DevExt, Srb);
    }

    // Check if this is a non-tagged request (QueueTag == SP_UNTAGGED or no queue action enabled)
    if (!((Srb->SrbFlags & SRB_FLAGS_QUEUE_ACTION_ENABLE) && (Srb->QueueTag != SP_UNTAGGED))) {
        // Non-tagged request - only one can be in flight at a time
        if (lun->NonTaggedInFlight) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Non-tagged flush rejected - another non-tagged request in flight\n");
#endif
            return ScsiBusy(DevExt, Srb);
        }
        // Mark that we now have a non-tagged request in flight
        lun->NonTaggedInFlight = Srb;
    }

    // Build command ID (flushes from SYNCHRONIZE_CACHE are standalone, not ORDERED tag flushes)
    commandId = NvmeBuildCommandId(DevExt, Srb);

    // Build NVMe Flush command
    memset(&nvmeCmd, 0, sizeof(NVME_COMMAND));
    nvmeCmd.CDW0.Fields.Opcode = NVME_CMD_FLUSH;
    nvmeCmd.CDW0.Fields.Flags = 0;
    nvmeCmd.CDW0.Fields.CommandId = commandId;
    nvmeCmd.NSID = lun->NamespaceId;

    // Submit the Flush command
    if (NvmeSubmitIoCommand(DevExt, &nvmeCmd)) {
        return ScsiPending(DevExt, Srb, 1);
    } else {
        // Submission failed
        lun->NonTaggedInFlight = NULL;
        return ScsiBusy(DevExt, Srb);
    }
}

//
// ScsiSubmitNoData - Submit a no-data I/O command (Verify) for an SRB
//
static BOOLEAN ScsiSubmitNoData(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PNVME_COMMAND Cmd)
{
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    PNVME_SRB_EXTENSION srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;

    if (!IsTagged(Srb)) {
        if (lun->NonTaggedInFlight) {
            return ScsiBusy(DevExt, Srb);
        }
        lun->NonTaggedInFlight = Srb;
    }
    srbExt->PrpListPage = 0xFF;
    Cmd->CDW0.Fields.CommandId = NvmeBuildCommandId(DevExt, Srb);
    Cmd->NSID = lun->NamespaceId;
    if (NvmeSubmitIoCommand(DevExt, Cmd)) {
        return ScsiPending(DevExt, Srb, 1);
    }
    if (!IsTagged(Srb)) {
        lun->NonTaggedInFlight = NULL;
    }
    return ScsiBusy(DevExt, Srb);
}

//
// ScsiHandleVerify - VERIFY(10/12/16): NVMe Verify when the controller has it,
// otherwise the medium is assumed readable (as before)
//
BOOLEAN ScsiHandleVerify(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    NVME_COMMAND cmd;
    ULONGLONG lba;
    ULONG count;
    BOOLEAN isWrite, fua;

    if (lun->SizeInBlocks == 0) {
        return ScsiBusy(DevExt, Srb);
    }
    ScsiGetLbaCount(Srb, &lba, &count, &isWrite, &fua);
    Srb->DataTransferLength = 0;
    if (count == 0) {
        return ScsiSuccess(DevExt, Srb);
    }
    if (lba >= lun->SizeInBlocks || count > lun->SizeInBlocks - lba) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    if (!(DevExt->Oncs & NVME_ONCS_VERIFY) || (Srb->Cdb[1] & 0x02)) {
        // No Verify command, or BYTCHK (compare with data we do not take)
        return ScsiSuccess(DevExt, Srb);
    }

    // One Verify covers at most 65536 blocks; longer requests are verified in front
    if (count > 0x10000) {
        count = 0x10000;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_CMD_VERIFY;
    cmd.CDW10 = (ULONG)(lba & 0xFFFFFFFF);
    cmd.CDW11 = (ULONG)(lba >> 32);
    cmd.CDW12 = count - 1;
    return ScsiSubmitNoData(DevExt, Srb, &cmd);
}

//
// ScsiHandleUnmap - UNMAP: block descriptors become Dataset Management ranges
// (deallocate), up to 256 per command
//
BOOLEAN ScsiHandleUnmap(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];
    PNVME_SRB_EXTENSION srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
    PUCHAR param = (PUCHAR)Srb->DataBuffer;
    PULONG ranges;
    NVME_COMMAND cmd;
    ULONG listLength, descLength, count, i, n;
    ULONGLONG lba;
    UCHAR page;

    if (!(DevExt->Oncs & NVME_ONCS_DSM)) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND_OPCODE, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    if (lun->SizeInBlocks == 0) {
        return ScsiBusy(DevExt, Srb);
    }

    listLength = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8];
    if (listLength > Srb->DataTransferLength) {
        listLength = Srb->DataTransferLength;
    }
    if (listLength == 0 || param == NULL) {
        return ScsiSuccess(DevExt, Srb);
    }
    if (listLength < 8) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_PARAM_LIST_LENGTH_ERROR, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    descLength = ((ULONG)param[2] << 8) | param[3];
    if (descLength > listLength - 8) {
        descLength = listLength - 8;
    }
    n = descLength / UNMAP_BLOCK_DESCRIPTOR_SIZE;
    if (n > UNMAP_MAX_DESCRIPTORS) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_PARAM, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }

    if (!IsTagged(Srb) && lun->NonTaggedInFlight) {
        return ScsiBusy(DevExt, Srb);
    }
    page = AllocatePrpListPage(DevExt);
    if (page == 0xFF) {
        return ScsiBusy(DevExt, Srb);
    }
    ranges = (PULONG)GetPrpListPageVirtual(DevExt, page);
    memset(ranges, 0, NVME_PAGE_SIZE);

    // DSM range: context attributes, length in blocks, starting LBA (64-bit)
    count = 0;
    for (i = 0; i < n; i++) {
        PUCHAR d = param + 8 + i * UNMAP_BLOCK_DESCRIPTOR_SIZE;
        ULONG blocks = ((ULONG)d[8] << 24) | ((ULONG)d[9] << 16) | ((ULONG)d[10] << 8) | d[11];
        lba = ((ULONGLONG)d[0] << 56) | ((ULONGLONG)d[1] << 48) | ((ULONGLONG)d[2] << 40) |
              ((ULONGLONG)d[3] << 32) | ((ULONGLONG)d[4] << 24) | ((ULONGLONG)d[5] << 16) |
              ((ULONGLONG)d[6] << 8) | d[7];
        if (blocks == 0) {
            continue;
        }
        if (lba >= lun->SizeInBlocks || blocks > lun->SizeInBlocks - lba) {
            FreePrpListPage(DevExt, page);
            ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            return ScsiError(DevExt, Srb, Srb->SrbStatus);
        }
        ranges[count * 4 + 1] = blocks;
        ranges[count * 4 + 2] = (ULONG)(lba & 0xFFFFFFFF);
        ranges[count * 4 + 3] = (ULONG)(lba >> 32);
        count++;
    }
    if (count == 0) {
        FreePrpListPage(DevExt, page);
        return ScsiSuccess(DevExt, Srb);
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.CDW0.Fields.Opcode = NVME_CMD_DSM;
    cmd.PRP1 = GetPrpListPagePhysical(DevExt, page).QuadPart;
    cmd.CDW10 = count - 1;
    cmd.CDW11 = NVME_DSM_DEALLOCATE;
    if (!ScsiSubmitNoData(DevExt, Srb, &cmd) || Srb->SrbStatus != SRB_STATUS_PENDING) {
        FreePrpListPage(DevExt, page);
        return TRUE;
    }
    srbExt->PrpListPage = page;     // freed by the I/O completion
    return TRUE;
}

//
// ScsiHandleStartStop - START STOP UNIT: stop / standby / idle = deepest
// non-operational power state, start / active = power state 0. The
// controller leaves a non-operational state by itself on the next I/O.
//
BOOLEAN ScsiHandleStartStop(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR pc = (UCHAR)(Srb->Cdb[4] >> 4);
    BOOLEAN start = (BOOLEAN)((Srb->Cdb[4] & 0x01) != 0);
    UCHAR target;

    switch (pc) {
        case 0x0:                       // START bit decides
            target = start ? 0 : DevExt->IdlePowerState;
            break;
        case 0x1:                       // active
            target = 0;
            break;
        case 0x2:                       // idle
        case 0x3:                       // standby
        case 0xA:                       // force idle
        case 0xB:                       // force standby
            target = DevExt->IdlePowerState;
            break;
        default:                        // LU control and the rest
            return ScsiSuccess(DevExt, Srb);
    }

    if (target != DevExt->PowerState && DevExt->Npss != 0) {
        NvmeSetPowerState(DevExt, target);
    }
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleModeSelect - MODE SELECT(6/10): caching page WCE -> Set Features
// Volatile Write Cache. Other pages are accepted unchanged.
//
BOOLEAN ScsiHandleModeSelect(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_MODE_SELECT10);
    PUCHAR data = (PUCHAR)Srb->DataBuffer;
    ULONG listLength, pos, pageLength, headerLength;
    BOOLEAN wce;

    listLength = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];
    if (listLength > Srb->DataTransferLength) {
        listLength = Srb->DataTransferLength;
    }
    if (listLength == 0 || data == NULL) {
        return ScsiSuccess(DevExt, Srb);
    }

    headerLength = ten ? 8 : 4;
    if (listLength < headerLength) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_PARAM_LIST_LENGTH_ERROR, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    pos = headerLength + (ten ? (((ULONG)data[6] << 8) | data[7]) : data[3]);

    while (pos + 2 <= listLength) {
        pageLength = (ULONG)data[pos + 1] + 2;
        if (pos + pageLength > listLength) {
            break;
        }
        if ((data[pos] & 0x3F) == MODE_PAGE_CACHING && pageLength >= 3) {
            wce = (BOOLEAN)((data[pos + 2] & 0x04) != 0);
            if (wce != DevExt->VwcEnabled) {
                if (!DevExt->VwcPresent || !NvmeSetVolatileWriteCache(DevExt, wce)) {
                    ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_PARAM, 0);
                    return ScsiError(DevExt, Srb, Srb->SrbStatus);
                }
            }
        }
        pos += pageLength;
    }
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleReportLuns - REPORT LUNS: one LUN per namespace
//
BOOLEAN ScsiHandleReportLuns(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR data = (PUCHAR)Srb->DataBuffer;
    ULONG allocation = ((ULONG)Srb->Cdb[6] << 24) | ((ULONG)Srb->Cdb[7] << 16) |
                       ((ULONG)Srb->Cdb[8] << 8) | Srb->Cdb[9];
    ULONG first = (ULONG)Srb->TargetId * NVMENT_LUNS_PER_TARGET;
    ULONG luns = (DevExt->LunCount > first) ? DevExt->LunCount - first : 0;
    ULONG length;
    ULONG i;

    // The namespaces of this target
    if (luns > NVMENT_LUNS_PER_TARGET) luns = NVMENT_LUNS_PER_TARGET;
    length = 8 + luns * 8;

    if (allocation > Srb->DataTransferLength) {
        allocation = Srb->DataTransferLength;
    }
    if (allocation < 16 || data == NULL) {
        ScsiSetSense(DevExt, Srb, SCSI_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0);
        return ScsiError(DevExt, Srb, Srb->SrbStatus);
    }
    memset(data, 0, allocation);
    data[0] = (UCHAR)((luns * 8) >> 24);
    data[1] = (UCHAR)((luns * 8) >> 16);
    data[2] = (UCHAR)((luns * 8) >> 8);
    data[3] = (UCHAR)(luns * 8);
    for (i = 0; i < luns && 8 + i * 8 + 8 <= allocation; i++) {
        data[8 + i * 8 + 1] = (UCHAR)i;   // single level, peripheral device addressing
    }
    Srb->DataTransferLength = (length < allocation) ? length : allocation;
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleReadDefectData10 - Handle SCSI READ DEFECT DATA (10) command
//
BOOLEAN ScsiHandleReadDefectData10(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR buffer;
    PCDB_READ_DEFECT_DATA cdb = (PCDB_READ_DEFECT_DATA)Srb->Cdb;
    ULONG allocationLength;
    UCHAR defectListFormat;
    BOOLEAN requestPList;
    BOOLEAN requestGList;

    // Parse CDB
    // Allocation length is at bytes 7-8 (big-endian)
    allocationLength = (cdb->AllocationLength[0] << 8) |
                       cdb->AllocationLength[1];

    // Defect list format byte (byte 2 for READ DEFECT DATA 10):
    // Bit 4: PLIST (request Primary defect list - factory defects)
    // Bit 3: GLIST (request Grown defect list - defects developed during use)
    // Bits 2-0: Format of defect descriptors
    requestPList = (cdb->FormatByte & 0x10) ? TRUE : FALSE;
    requestGList = (cdb->FormatByte & 0x08) ? TRUE : FALSE;
    defectListFormat = cdb->FormatByte & 0x07;

    // Check buffer size - need at least 4 bytes for header
    if (Srb->DataTransferLength < 4) {
        return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
    }

    buffer = (PUCHAR)Srb->DataBuffer;
    memset(buffer, 0, Srb->DataTransferLength);

    // Build READ DEFECT DATA (10) response header (4 bytes)
    // Per SBC-3 specification section 5.7

    // Byte 0: Reserved
    buffer[0] = 0x00;

    // Byte 1: Response flags
    // Bit 7: PS (Parameters Saveable) = 0 (not saveable)
    // Bit 4: PLIST = echo from CDB (indicates if P-list is being returned)
    // Bit 3: GLIST = echo from CDB (indicates if G-list is being returned)
    // Bits 2-0: Defect List Format = echo from CDB
    // For NVMe SSDs: We acknowledge the request but return empty lists
    buffer[1] = (cdb->FormatByte & 0x1F);  // Copy PLIST, GLIST, and format bits

    // Bytes 2-3: Defect list length (big-endian, excludes the 4-byte header)
    // For NVMe SSDs, both P-list and G-list are empty (length = 0)
    // NVMe drives handle bad blocks transparently via internal wear leveling
    buffer[2] = 0x00;
    buffer[3] = 0x00;

    // Set actual transfer length (just the 4-byte header, no defect descriptors)
    Srb->DataTransferLength = 4;
    return ScsiSuccess(DevExt, Srb);
}

//
// ScsiHandleModeSense - Handle SCSI MODE SENSE(6) and MODE SENSE(10) commands
//
BOOLEAN ScsiHandleModeSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PCDB cdb = (PCDB)Srb->Cdb;
    PUCHAR buffer;
    ULONG allocationLength;
    UCHAR pageCode;
    UCHAR pageControl;
    BOOLEAN dbd;  // Disable Block Descriptors
    BOOLEAN isModeSense10;
    ULONG offset;
    ULONG headerSize;
    ULONG blockDescLength;
    ULONG totalLength;
    ULONG pageStart;
    PNVME_LUN lun = &DevExt->Luns[NVME_LUN_INDEX(Srb)];

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: ScsiHandleModeSense called DataTransferLength:%d\n", Srb->DataTransferLength);
#endif

    buffer = (PUCHAR)Srb->DataBuffer;
    isModeSense10 = (Srb->Cdb[0] == SCSIOP_MODE_SENSE10);

    // Parse the CDB based on command type
    if (isModeSense10) {
        pageCode = cdb->MODE_SENSE10.PageCode;
        pageControl = cdb->MODE_SENSE10.Pc;
        dbd = cdb->MODE_SENSE10.Dbd;
        allocationLength = (cdb->MODE_SENSE10.AllocationLength[0] << 8) | cdb->MODE_SENSE10.AllocationLength[1];
        headerSize = 8;  // MODE SENSE(10) header is 8 bytes
    } else {
        pageCode = cdb->MODE_SENSE.PageCode;
        pageControl = cdb->MODE_SENSE.Pc;
        dbd = cdb->MODE_SENSE.Dbd;
        allocationLength = cdb->MODE_SENSE.AllocationLength;
        headerSize = 4;  // MODE SENSE(6) header is 4 bytes
    }

    // Check buffer size
    if (Srb->DataTransferLength < headerSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: ModeSense buffer too small\n");
#endif
        return ScsiError(DevExt, Srb, SRB_STATUS_DATA_OVERRUN);
    }

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: ModeSense pageCode=0x%02X pageControl=0x%02X dbd=%d\n",
                   pageCode, pageControl, dbd);
#endif

    // If unsupported page requested and not "return all", return error
    if (pageCode != MODE_SENSE_RETURN_ALL &&
        pageCode != MODE_PAGE_FORMAT_DEVICE &&
        pageCode != MODE_PAGE_CACHING &&
        pageCode != MODE_PAGE_CONTROL &&
        pageCode != MODE_PAGE_RIGID_GEOMETRY &&
        pageCode != MODE_PAGE_POWER_CONDITION &&
        pageCode != MODE_PAGE_FAULT_REPORTING) {
        return ScsiError(DevExt, Srb, SRB_STATUS_INVALID_REQUEST);
    }

    // Clear the buffer
    memset(buffer, 0, Srb->DataTransferLength);

    // Determine block descriptor length
    // For simplicity, we include a block descriptor unless DBD is set
    blockDescLength = dbd ? 0 : 8;

    offset = headerSize;
#if 1
    // Add block descriptor if not disabled
    if (!dbd && (offset + blockDescLength) <= Srb->DataTransferLength) {
        PUCHAR blockDesc = buffer + offset;
        ULONG blockSize = lun->BlockSize ? lun->BlockSize : 512;
        ULONGLONG numBlocks = lun->SizeInBlocks;

        // Block descriptor format (8 bytes):
        // Byte 0: Density code (0 = default)
        blockDesc[0] = 0x00;

        // Bytes 1-3: Number of blocks (or 0xFFFFFF if > 24-bit)
        if (numBlocks > 0xFFFFFF) {
            blockDesc[1] = 0xFF;
            blockDesc[2] = 0xFF;
            blockDesc[3] = 0xFF;
        } else {
            blockDesc[1] = (UCHAR)((numBlocks >> 16) & 0xFF);
            blockDesc[2] = (UCHAR)((numBlocks >> 8) & 0xFF);
            blockDesc[3] = (UCHAR)(numBlocks & 0xFF);
        }

        // Byte 4: Reserved
        blockDesc[4] = 0x00;

        // Bytes 5-7: Block length (big-endian)
        blockDesc[5] = (UCHAR)((blockSize >> 16) & 0xFF);
        blockDesc[6] = (UCHAR)((blockSize >> 8) & 0xFF);
        blockDesc[7] = (UCHAR)(blockSize & 0xFF);

        offset += blockDescLength;
    }
#endif
#if 1
    // Add mode pages based on pageCode. For changeable values the pages are
    // zeroed afterwards; only WCE (caching page) can be changed.
    pageStart = offset;
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_FORMAT_DEVICE) {
        // Page 03h: Format Device Page
        if ((offset + 24) <= Srb->DataTransferLength) {
            PUCHAR formatPage = buffer + offset;
            ULONG sectorsPerTrack = 63;
            ULONG blockSize = lun->BlockSize ? lun->BlockSize : 512;

            formatPage[0] = MODE_PAGE_FORMAT_DEVICE;  // Page code
            formatPage[1] = 22;  // Page length (n-1, total 24 bytes)

            // Bytes 2-3: Tracks per zone (big-endian, 0 = not reported)
            formatPage[2] = 0x00;
            formatPage[3] = 0x00;

            // Bytes 4-5: Alternate sectors per zone (big-endian, 0 = not reported)
            formatPage[4] = 0x00;
            formatPage[5] = 0x00;

            // Bytes 6-7: Alternate tracks per zone (big-endian, 0 = not reported)
            formatPage[6] = 0x00;
            formatPage[7] = 0x00;

            // Bytes 8-9: Alternate tracks per logical unit (big-endian, 0 = not reported)
            formatPage[8] = 0x00;
            formatPage[9] = 0x00;

            // Bytes 10-11: Sectors per track (big-endian)
            formatPage[10] = (UCHAR)((sectorsPerTrack >> 8) & 0xFF);
            formatPage[11] = (UCHAR)(sectorsPerTrack & 0xFF);

            // Bytes 12-13: Data bytes per physical sector (big-endian)
            formatPage[12] = (UCHAR)((blockSize >> 8) & 0xFF);
            formatPage[13] = (UCHAR)(blockSize & 0xFF);

            // Bytes 14-15: Interleave (big-endian, 0 = default)
            formatPage[14] = 0x00;
            formatPage[15] = 0x00;

            // Bytes 16-17: Track skew factor (big-endian, 0 = default)
            formatPage[16] = 0x00;
            formatPage[17] = 0x00;

            // Bytes 18-19: Cylinder skew factor (big-endian, 0 = default)
            formatPage[18] = 0x00;
            formatPage[19] = 0x00;

            // Byte 20: Flags
            // Bit 7: SSEC (Soft Sector) = 1 (soft sectored)
            // Bit 6: HSEC (Hard Sector) = 0
            // Bit 5: RMB (Removable) = 0
            // Bit 4: SURF (Surface) = 0
            // Bits 3-0: Reserved
            formatPage[20] = 0x80;  // SSEC=1 (soft sectored)

            // Bytes 21-23: Reserved
            formatPage[21] = 0x00;
            formatPage[22] = 0x00;
            formatPage[23] = 0x00;

            offset += 24;
        }
    }
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_CACHING) {
        // Page 08h: Caching Parameters Page
        if ((offset + 20) <= Srb->DataTransferLength) {
            PUCHAR cachePage = buffer + offset;

            cachePage[0] = MODE_PAGE_CACHING;  // Page code
            cachePage[1] = 18;  // Page length (n-1, total 20 bytes)

            // Byte 2: Flags
            // IC (Initiator Control) = 0
            // ABPF (Abort Pre-Fetch) = 0
            // CAP (Caching Analysis Permitted) = 0
            // DISC (Discontinuity) = 0
            // SIZE (Size enable) = 0
            // WCE (Write Cache Enable) = 1 (NVMe typically has write cache)
            // MF (Multiplication Factor) = 0
            // RCD (Read Cache Disable) = 0 (read cache enabled)
            cachePage[2] = DevExt->VwcEnabled ? 0x04 : 0x00;  // WCE = volatile write cache enabled

            // Byte 3: Read retention priority (4 bits) and Write retention priority (4 bits)
            cachePage[3] = 0x00;  // Equal priority

            // Bytes 4-5: Disable pre-fetch transfer length (big-endian)
            cachePage[4] = 0x00;
            cachePage[5] = 0x00;

            // Bytes 6-7: Minimum pre-fetch (big-endian)
            cachePage[6] = 0x00;
            cachePage[7] = 0x00;

            // Bytes 8-9: Maximum pre-fetch (big-endian)
            cachePage[8] = 0xFF;
            cachePage[9] = 0xFF;

            // Bytes 10-11: Maximum pre-fetch ceiling (big-endian)
            cachePage[10] = 0xFF;
            cachePage[11] = 0xFF;

            // Byte 12: Flags
            // FSW (Force Sequential Write) = 0
            // LBCSS (Logical Block Cache Segment Size) = 0
            // DRA (Disable Read Ahead) = 0
            // NV_DIS (Non-Volatile Cache Disable) = 0
            cachePage[12] = 0x00;

            // Byte 13: Number of cache segments
            cachePage[13] = 0x00;

            // Bytes 14-15: Cache segment size (big-endian)
            cachePage[14] = 0x00;
            cachePage[15] = 0x00;

            // Bytes 16-19: Reserved
            cachePage[16] = 0x00;
            cachePage[17] = 0x00;
            cachePage[18] = 0x00;
            cachePage[19] = 0x00;

            offset += 20;
        }
    }
#endif
#if 1
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_RIGID_GEOMETRY) {
        // Page 04h: Rigid Disk Geometry Page
        if ((offset + 24) <= Srb->DataTransferLength) {
            PUCHAR geometryPage = buffer + offset;
            ULONGLONG totalCylinders;
            ULONG sectorsPerTrack = 63;
            ULONG heads = 64;
            ULONGLONG numBlocks = lun->SizeInBlocks;
            ULONG blockSize = lun->BlockSize ? lun->BlockSize : 512;

            // Calculate number of cylinders based on: Total Blocks = Cylinders * Heads * Sectors
            // Cylinders = Total Blocks / (Heads * Sectors)
            if (numBlocks > 0) {
                totalCylinders = numBlocks / (heads * sectorsPerTrack);
                if (totalCylinders == 0) {
                    totalCylinders = 1;  // Minimum 1 cylinder
                }
            } else {
                totalCylinders = 1;
            }

            // Clamp to 24-bit max (16777215 cylinders)
            if (totalCylinders > 0xFFFFFF) {
                totalCylinders = 0xFFFFFF;
            }

            geometryPage[0] = MODE_PAGE_RIGID_GEOMETRY;  // Page code
            geometryPage[1] = 22;  // Page length (n-1, total 24 bytes)

            // Bytes 2-4: Number of cylinders (24-bit big-endian)
            geometryPage[2] = (UCHAR)((totalCylinders >> 16) & 0xFF);
            geometryPage[3] = (UCHAR)((totalCylinders >> 8) & 0xFF);
            geometryPage[4] = (UCHAR)(totalCylinders & 0xFF);

            // Byte 5: Number of heads
            geometryPage[5] = (UCHAR)heads;

            // Bytes 6-8: Starting cylinder - write precompensation (obsolete, set to 0)
            geometryPage[6] = 0x00;
            geometryPage[7] = 0x00;
            geometryPage[8] = 0x00;

            // Bytes 9-11: Starting cylinder - reduced write current (obsolete, set to 0)
            geometryPage[9] = 0x00;
            geometryPage[10] = 0x00;
            geometryPage[11] = 0x00;

            // Bytes 12-13: Device step rate (big-endian, 0 = default)
            geometryPage[12] = 0x00;
            geometryPage[13] = 0x00;

            // Bytes 14-16: Landing zone cylinder (obsolete, set to 0)
            geometryPage[14] = 0x00;
            geometryPage[15] = 0x00;
            geometryPage[16] = 0x00;

            // Byte 17: RPL (Rotational Position Locking)
            // Bits 1-0: 00b = None
            geometryPage[17] = 0x00;

            // Byte 18: Rotational offset (0 = not supported)
            geometryPage[18] = 0x00;

            // Byte 19: Reserved
            geometryPage[19] = 0x00;

            // Bytes 20-21: Medium rotation rate (big-endian)
            // 0x0000 = Not reported
            // 0x0001 = Non-rotating media (SSD)
            // 0x0002-0x0400 = Reserved
            // 0x0401-0xFFFE = Rotations per minute
            // 0xFFFF = Reserved
            // For SSDs, we should use 0x0001
            geometryPage[20] = 0x00;
            geometryPage[21] = 0x01;  // Non-rotating media (SSD)

            // Bytes 22-23: Reserved
            geometryPage[22] = 0x00;
            geometryPage[23] = 0x00;

            offset += 24;
        }
    }
#endif
#if 1
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_CONTROL) {
        // Page 0Ah: Control Mode Page
        if ((offset + 12) <= Srb->DataTransferLength) {
            PUCHAR controlPage = buffer + offset;

            controlPage[0] = MODE_PAGE_CONTROL;  // Page code
            controlPage[1] = 10;  // Page length (n-1, total 12 bytes)

            // Byte 2: TST (Task Set Type), TMF_ONLY, etc.
            controlPage[2] = 0x00;

            // Byte 3: QERR (Queue Error Management), etc.
            // Bits 1-0: QERR=00b (restricted reordering - safest default)
            // Bits 3-2: Reserved = 0
            // Bits 5-4: Queue Algorithm Modifier = 0 (restricted reordering)
            // Bits 7-6: Reserved = 0
            controlPage[3] = 0x00;  // QERR=0, all other bits=0

            // Byte 4: Flags (ATO, TAS, AUTOLOAD MODE, etc.)
            controlPage[4] = 0x00;

            // Bytes 5-6: Reserved
            controlPage[5] = 0x00;
            controlPage[6] = 0x00;

            // Bytes 7-8: Busy timeout period (big-endian)
            controlPage[7] = 0x00;
            controlPage[8] = 0x00;

            // Bytes 9-10: Extended self-test completion time (big-endian)
            controlPage[9] = 0x00;
            controlPage[10] = 0x00;

            // Byte 11: Reserved
            controlPage[11] = 0x00;

            offset += 12;
        }
    }
#endif
#if 1
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_POWER_CONDITION) {
        // Page 1Ah: Power Condition Mode Page
        if ((offset + 12) <= Srb->DataTransferLength) {
            PUCHAR powerPage = buffer + offset;

            powerPage[0] = MODE_PAGE_POWER_CONDITION;  // Page code
            powerPage[1] = 10;  // Page length (n-1, total 12 bytes)

            // Byte 2: Reserved
            powerPage[2] = 0x00;

            // Byte 3: Flags (STANDBY, IDLE)
            // All power conditions disabled (NVMe manages power internally)
            powerPage[3] = 0x00;

            // Bytes 4-7: Idle condition timer (big-endian, in 100ms units)
            powerPage[4] = 0x00;
            powerPage[5] = 0x00;
            powerPage[6] = 0x00;
            powerPage[7] = 0x00;

            // Bytes 8-11: Standby condition timer (big-endian, in 100ms units)
            powerPage[8] = 0x00;
            powerPage[9] = 0x00;
            powerPage[10] = 0x00;
            powerPage[11] = 0x00;

            offset += 12;
        }
    }
#endif
#if 1
    if (pageCode == MODE_SENSE_RETURN_ALL || pageCode == MODE_PAGE_FAULT_REPORTING) {
        // Page 1Ch: Informational Exceptions Control (IEC) Page
        // This page indicates SMART-like health monitoring capability
        if ((offset + 12) <= Srb->DataTransferLength) {
            PUCHAR iecPage = buffer + offset;

            iecPage[0] = MODE_PAGE_FAULT_REPORTING;  // Page code 0x1C
            iecPage[1] = 10;  // Page length (n-1, total 12 bytes)

            // Byte 2: Flags
            // Bit 7: PERF (Performance) = 0
            // Bit 6: EBF (Enable Background Function) = 0
            // Bit 5: EWASC (Enable Warning) = 0
            // Bit 4: DEXCPT (Disable Exceptions) = 0 (enabled - we support SMART)
            // Bit 3: TEST = 0
            // Bit 2: EBACKERR = 0
            // Bit 1: Reserved
            // Bit 0: LOGERR (Log Errors) = 0
            iecPage[2] = 0x00;  // DEXCPT=0 means informational exceptions enabled

            // Byte 3: MRIE (Method of Reporting Informational Exceptions)
            // 0h = No reporting of informational exception conditions
            // 2h = Generate unit attention
            // 3h = Conditionally generate recovered error
            // 4h = Unconditionally generate recovered error
            // 5h = Generate no sense
            // 6h = Only report informational exception on request (like SMART)
            iecPage[3] = 0x06;  // Report on request (LOG SENSE)

            // Bytes 4-7: Interval Timer (big-endian, in 100ms units)
            // 0 = vendor specific
            iecPage[4] = 0x00;
            iecPage[5] = 0x00;
            iecPage[6] = 0x00;
            iecPage[7] = 0x00;

            // Bytes 8-11: Report Count (big-endian)
            // Number of times to report an exception
            // 0 = unlimited
            iecPage[8] = 0x00;
            iecPage[9] = 0x00;
            iecPage[10] = 0x00;
            iecPage[11] = 0x01;  // Report once

            offset += 12;
        }
    }
#endif
    // Changeable values: page bodies zero, except WCE when there is a write cache
    if (pageControl == MODE_SENSE_CHANGEABLE_VALUES) {
        ULONG pos = pageStart;
        while (pos + 2 <= offset) {
            ULONG len = (ULONG)buffer[pos + 1] + 2;
            memset(buffer + pos + 2, 0, len - 2);
            if (buffer[pos] == MODE_PAGE_CACHING && DevExt->VwcPresent) {
                buffer[pos + 2] = 0x04;
            }
            pos += len;
        }
    }

    // Calculate total length
    totalLength = offset;

    // Fill in the mode parameter header
    if (isModeSense10) {
        // MODE SENSE(10) header
        USHORT modeDataLength = (USHORT)(totalLength - 2);  // Length excludes the length field itself

        buffer[0] = (UCHAR)((modeDataLength >> 8) & 0xFF);
        buffer[1] = (UCHAR)(modeDataLength & 0xFF);
        buffer[2] = 0x00;  // Medium type (0 = default)
        buffer[3] = 0x10;  // Device-specific parameter: DPOFUA (FUA honoured)
        buffer[4] = 0x00;  // Reserved
        buffer[5] = 0x00;  // Reserved
        buffer[6] = (UCHAR)((blockDescLength >> 8) & 0xFF);
        buffer[7] = (UCHAR)(blockDescLength & 0xFF);
    } else {
        // MODE SENSE(6) header
        UCHAR modeDataLength = (UCHAR)(totalLength - 1);  // Length excludes the length field itself

        if (modeDataLength > 0xFF) {
            modeDataLength = 0xFF;  // Truncate for MODE SENSE(6)
        }

        buffer[0] = modeDataLength;
        buffer[1] = 0x00;  // Medium type (0 = default)
        buffer[2] = 0x10;  // Device-specific parameter: DPOFUA (FUA honoured)
        buffer[3] = (UCHAR)blockDescLength;
    }

    // Update transfer length
    if (totalLength > allocationLength) {
        totalLength = allocationLength;
    }
    if (totalLength > Srb->DataTransferLength) {
        totalLength = Srb->DataTransferLength;
    }

    Srb->DataTransferLength = totalLength;
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: ModeSense completed, returning %d bytes\n", totalLength);
#endif
    return ScsiSuccess(DevExt, Srb);
}

//
// HandleIO_NVMENT - Process NVMENT custom IOCTLs
//
BOOLEAN HandleIO_NVMENT(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PSRB_IO_CONTROL srbControl;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HandleIO_NVMENT called - DataTransferLength=%u\n",
                   Srb->DataTransferLength);
#endif

    srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NVMENT IOCTL ControlCode=0x%08X Length=%u\n",
                   srbControl->ControlCode, srbControl->Length);
#endif

    switch (srbControl->ControlCode) {
        case NVMENT_IOCTL_QUERY_INFO:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT QUERY_INFO\n");
#endif
            // Optional NVMENT_INFO after the header (older callers send none)
            if (srbControl->Length >= sizeof(NVMENT_INFO) &&
                Srb->DataTransferLength >= sizeof(SRB_IO_CONTROL) + sizeof(NVMENT_INFO)) {
                PNVMENT_INFO info = (PNVMENT_INFO)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));
                ULONG l;

                memset(info, 0, sizeof(NVMENT_INFO));
                info->Version = NVMENT_DRIVER_VERSION;
                info->NvmeVersion = DevExt->Version;
                info->Flags = (DevExt->PollMode ? NVMENT_INFO_POLLING : 0) |
                              (DevExt->Stopped ? NVMENT_INFO_POWERED_DOWN : 0) |
                              (DevExt->VwcEnabled ? NVMENT_INFO_VWC : 0) |
                              (DevExt->TrimEnable ? NVMENT_INFO_TRIM_MODE : 0) |
                              (DevExt->ApstOn ? NVMENT_INFO_APST : 0);
                info->LunCount = DevExt->LunCount;
                info->PowerState = DevExt->PowerState;
                info->MaxTransfer = DevExt->MaxTransferSizeBytes;
                info->QueueDepthMax = DevExt->MaxQueueDepthReached;
                info->InterruptCount = DevExt->InterruptCount;
                for (l = 0; l < NVMENT_INFO_LUNS; l++) {
                    info->NamespaceId[l] = DevExt->Luns[l].NamespaceId;
                    info->BlockSize[l] = DevExt->Luns[l].BlockSize;
                    info->SizeInBlocks[l] = DevExt->Luns[l].SizeInBlocks;
                }
            }
            srbControl->ReturnCode = 0;  // Success
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;

        case NVMENT_IOCTL_POWER_DOWN:
            // For an ACPI/APM driver on NT 3.1-4.0: shutdown notification
            // (the controller writes its volatile cache back). The next
            // request or POWER_UP sets the controller up again.
            NvmePowerDown(DevExt);
            srbControl->ReturnCode = 0;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;

        case NVMENT_IOCTL_POWER_UP:
            srbControl->ReturnCode = (DevExt->Stopped && !NvmeReinitialize(DevExt)) ? 1 : 0;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return TRUE;

        case NVMENT_IOCTL_TRIM_MODE_ON:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT TRIM_MODE_ON (Length=%u)\n", srbControl->Length);
#endif
            // Validate that we have the 4KB pattern buffer
            if (srbControl->Length != 4096 ||
                Srb->DataTransferLength < sizeof(SRB_IO_CONTROL) + 4096) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NVMENT TRIM_MODE_ON invalid length (expected 4096, got %u)\n",
                               srbControl->Length);
#endif
                srbControl->ReturnCode = 1;  // Error
                Srb->SrbStatus = SRB_STATUS_ERROR;
                return FALSE;
            }

            if (DevExt->TrimPattern == NULL) {
                srbControl->ReturnCode = 1;
                Srb->SrbStatus = SRB_STATUS_ERROR;
                return FALSE;
            }

            // Copy the 4KB pattern to device extension
            memcpy(
                DevExt->TrimPattern,
                (PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL),
                4096
            );

            // Enable TRIM mode
            DevExt->TrimEnable = TRUE;

#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT TRIM mode enabled, pattern stored\n");
#endif
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            srbControl->ReturnCode = 0;  // Success
            return TRUE;

        case NVMENT_IOCTL_TRIM_MODE_OFF:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT TRIM_MODE_OFF\n");
#endif
            // Disable TRIM mode
            DevExt->TrimEnable = FALSE;

#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT TRIM mode disabled\n");
#endif
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            srbControl->ReturnCode = 0;  // Success
            return TRUE;

        default:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NVMENT unknown ControlCode: 0x%08X\n", srbControl->ControlCode);
#endif
            srbControl->ReturnCode = 1;  // Error
            // Don't set SRB status - let caller set SRB_STATUS_INVALID_REQUEST
            return FALSE;
    }
}

//
// HandleIO_NvmeMini - Process NvmeMini IOCTLs (NVMe passthrough)
//
// This handler processes NVMe passthrough commands via SRB_IO_CONTROL using
// the NVME_PASS_THROUGH structure. The buffer layout is:
//   [SRB_IO_CONTROL (28 bytes)] + [NVME_PASS_THROUGH] + [data buffer]
//
BOOLEAN HandleIO_NvmeMini(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PSRB_IO_CONTROL srbControl;
    PNVME_PASS_THROUGH nvmePassThru;
    PULONG nvmeCmd;
    UCHAR nvmeOpcode;
    ULONG namespaceId;
    UCHAR parameter;
    USHORT commandId;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HandleIO_NvmeMini called - DataTransferLength=%u\n",
                   Srb->DataTransferLength);
#endif

    srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NvmeMini IOCTL ControlCode=0x%08X Length=%u\n",
                   srbControl->ControlCode, srbControl->Length);
#endif

    // Get NVME_PASS_THROUGH structure (follows SRB_IO_CONTROL)
    nvmePassThru = (PNVME_PASS_THROUGH)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));
    nvmeCmd = nvmePassThru->Command;  // 16 DWORDs (64 bytes) NVMe command

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NVME_PASS_THROUGH - QueueId=%u Direction=%u DataBufferLen=%u MetaDataLen=%u ReturnBufferLen=%u\n",
                   nvmePassThru->QueueId, nvmePassThru->Direction, nvmePassThru->DataBufferLen,
                   nvmePassThru->MetaDataLen, nvmePassThru->ReturnBufferLen);
#endif

    // Verify this is an Admin queue command (QueueId == 0)
    if (nvmePassThru->QueueId != 0) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: HandleIO_NvmeMini not Admin queue (QueueId=%u)\n",
                       nvmePassThru->QueueId);
#endif
        return FALSE;
    }

    // Extract NVMe command parameters from the 64-byte command
    // CDW0 bits 0-7 = opcode
    nvmeOpcode = (UCHAR)(nvmeCmd[0] & 0xFF);
    // CDW1 = namespace ID
    namespaceId = nvmeCmd[1];

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: NVMe Admin command - Opcode=%02X NSID=0x%08X\n",
                   nvmeOpcode, namespaceId);
#endif

    // Process based on NVMe opcode
    switch (nvmeOpcode) {
        case NVME_ADMIN_IDENTIFY:
            // IDENTIFY command
            // CDW10 bits 0-7 = CNS (Controller or Namespace Structure)
            // IDENTIFY always returns 4KB of data
            parameter = (UCHAR)(nvmeCmd[10] & 0xFF);
            commandId = NvmeBuildAdminCommandId(DevExt, Srb, ADMIN_KIND_USER);

#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NvmeMini IDENTIFY - NSID=%u CNS=%02X CDW10=%08X CID=%04X\n",
                           namespaceId, parameter, nvmeCmd[10], commandId);
#endif

            if (!NvmeIdentifyEx(DevExt, namespaceId, parameter, commandId, Srb)) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NvmeIdentifyEx failed\n");
#endif
                srbControl->ReturnCode = 1;  // Error
                Srb->SrbStatus = SRB_STATUS_ERROR;
                return FALSE;
            }

            // Mark as pending - will complete in interrupt handler
            Srb->SrbStatus = SRB_STATUS_PENDING;
            srbControl->ReturnCode = 0;  // Success (will be completed async)
            return TRUE;

        case NVME_ADMIN_GET_LOG_PAGE:
            // GET_LOG_PAGE command
            // CDW10 bits 0-7 = Log Page ID
            // CDW10 bits 31:16 = NUMDL (Number of Dwords Lower)
            {
                ULONG numDwords;
                ULONG numdl = (nvmeCmd[10] >> 16) & 0xFFFF;

                parameter = (UCHAR)(nvmeCmd[10] & 0xFF);
                commandId = NvmeBuildAdminCommandId(DevExt, Srb, ADMIN_KIND_USER);

#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NvmeMini GET_LOG_PAGE - NSID=%u LID=%02X CDW10=%08X (NUMDL=%u) CID=%04X\n",
                               namespaceId, parameter, nvmeCmd[10], numdl, commandId);
#endif

                // Extract NUMDL and convert to NumDwords (NUMDL+1)
                numDwords = numdl + 1;

                // Sanity check: Fix obviously wrong NUMDL values based on known log page sizes
                // Some applications (like SIV) may set incorrect NUMDL values
                switch (parameter) {
                    case 0x01:  // Error Information log - 64 bytes per entry, typically request 256 bytes = 64 DWORDs
                        if (numDwords < 64) {
                            numDwords = 64;
#ifdef NVMENT_DBG
                            ScsiDebugPrint(0, "NVMENT: Fixing NUMDL for Error log (was %u, now 64 DWORDs)\n", numdl + 1);
#endif
                        }
                        break;
                    case 0x02:  // SMART/Health Information log - always 512 bytes = 128 DWORDs
                        if (numDwords != 128) {
                            numDwords = 128;
#ifdef NVMENT_DBG
                            ScsiDebugPrint(0, "NVMENT: Fixing NUMDL for SMART log (was %u, now 128 DWORDs)\n", numdl + 1);
#endif
                        }
                        break;
                    case 0x03:  // Firmware Slot Information log - 512 bytes = 128 DWORDs
                        if (numDwords != 128) {
                            numDwords = 128;
#ifdef NVMENT_DBG
                            ScsiDebugPrint(0, "NVMENT: Fixing NUMDL for Firmware Slot log (was %u, now 128 DWORDs)\n", numdl + 1);
#endif
                        }
                        break;
                    default:
                        // For unknown log pages, use what the application specified
                        // but warn if it seems suspiciously small
                        if (numDwords < 16) {
#ifdef NVMENT_DBG
                            ScsiDebugPrint(0, "NVMENT: WARNING - Small NUMDL=%u for log page 0x%02X\n", numdl, parameter);
#endif
                        }
                        break;
                }

                if (!NvmeGetLogPageEx(DevExt, Srb, parameter, namespaceId, commandId, numDwords)) {
#ifdef NVMENT_DBG
                    ScsiDebugPrint(0, "NVMENT: NvmeGetLogPageEx failed\n");
#endif
                    srbControl->ReturnCode = 1;  // Error
                    Srb->SrbStatus = SRB_STATUS_ERROR;
                    return FALSE;
                }

                // Mark as pending - will complete in interrupt handler
                Srb->SrbStatus = SRB_STATUS_PENDING;
                srbControl->ReturnCode = 0;  // Success (will be completed async)
                return TRUE;
            }

        default:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: NvmeMini unsupported NVMe admin opcode: %02X\n", nvmeOpcode);
#endif
            srbControl->ReturnCode = 1;  // Error
            return FALSE;
    }
}

//
// SMART IOCTL Handler Functions
//

BOOLEAN HandleSmartGetVersion(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PSRB_IO_CONTROL srbControl)
{
    PGETVERSIONINPARAMS versionParams;
    ULONG requiredSize = sizeof(SRB_IO_CONTROL) + sizeof(GETVERSIONINPARAMS);

    if (Srb->DataTransferLength < requiredSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: SMART_VERSION buffer too small (%u < %u)\n",
                       Srb->DataTransferLength, requiredSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;
    }

    versionParams = (PGETVERSIONINPARAMS)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));
    memset(versionParams, 0, sizeof(GETVERSIONINPARAMS));

    // Fill in version information
    versionParams->bVersion = 1;      // Driver version 1.x
    versionParams->bRevision = 0;     // Revision 0
    versionParams->bReserved = 0;

    // For NVMe, we emulate a single IDE device on primary controller
    // Bit 0 = device 0 on primary controller
    versionParams->bIDEDeviceMap = 0x01;

    // We support ATA ID and SMART commands via emulation
    versionParams->fCapabilities = CAP_ATA_ID_CMD | CAP_SMART_CMD;

    // Success
    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL) + sizeof(GETVERSIONINPARAMS);
    srbControl->ReturnCode = 0;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    return TRUE;
}

BOOLEAN HandleSmartIdentify(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PSRB_IO_CONTROL srbControl)
{
    PSENDCMDINPARAMS sendCmdIn;
    PSENDCMDOUTPARAMS sendCmdOut;
    ULONG requiredSize;
    ULONG dataBufferSize;

    // Validate buffer size for SENDCMDINPARAMS input
    requiredSize = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDINPARAMS) - 1;
    if (Srb->DataTransferLength < requiredSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: IDENTIFY buffer too small for input (%u < %u)\n",
                       Srb->DataTransferLength, requiredSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;
    }

    // Validate buffer size for IDENTIFY output (512 bytes)
    dataBufferSize = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) + 512 - 1;
    if (Srb->DataTransferLength < dataBufferSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: IDENTIFY buffer too small for output (%u < %u)\n",
                       Srb->DataTransferLength, dataBufferSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;
    }

    sendCmdIn = (PSENDCMDINPARAMS)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));
    sendCmdOut = (PSENDCMDOUTPARAMS)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));

    // Generate ATA IDENTIFY data from NVMe controller/namespace info
    NvmeToAtaIdentify(DevExt, (PATA_IDENTIFY_DEVICE_STRUCT)sendCmdOut->bBuffer);

    // Set driver status
    memset(&sendCmdOut->DriverStatus, 0, sizeof(DRIVERSTATUS));
    sendCmdOut->DriverStatus.bDriverError = 0;
    sendCmdOut->DriverStatus.bIDEError = 0;
    sendCmdOut->cBufferSize = 512;

    // Success
    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) + 512 - 1;
    srbControl->ReturnCode = 0;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    return TRUE;
}

BOOLEAN HandleSmartReadAttribs(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PSRB_IO_CONTROL srbControl)
{
    ULONG requiredSize;
    ULONG dataBufferSize;

    // Validate buffer size for SENDCMDINPARAMS input
    requiredSize = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDINPARAMS) - 1;
    if (Srb->DataTransferLength < requiredSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: READ_SMART_ATTRIBS buffer too small for input (%u < %u)\n",
                       Srb->DataTransferLength, requiredSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;    
    }

    // Validate buffer size for SMART output (512 bytes)
    dataBufferSize = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) + 512 - 1;
    if (Srb->DataTransferLength < dataBufferSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: READ_SMART_ATTRIBS buffer too small for output (%u < %u)\n",
                       Srb->DataTransferLength, dataBufferSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;
    }

    // Issue async NVMe Get Log Page command for SMART/Health info
    // The completion handler will convert NVMe SMART to ATA SMART format
    if (!NvmeGetLogPage(DevExt, Srb, NVME_LOG_PAGE_SMART_HEALTH)) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Failed to submit Get Log Page for SMART attributes\n");
#endif
        return FALSE;
    }

    // Mark SRB as pending - will be completed in interrupt handler
    Srb->SrbStatus = SRB_STATUS_PENDING;
    ScsiPortNotification(NextRequest, DevExt, NULL);
    return TRUE;
}

BOOLEAN HandleSmartEnableDisable(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PSRB_IO_CONTROL srbControl,
    IN BOOLEAN Enable)
{
    // Enable or disable SMART monitoring
    DevExt->SMARTEnabled = Enable;
    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL);
    srbControl->ReturnCode = 0;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    return TRUE;
}

BOOLEAN HandleSmartReturnStatus(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PSRB_IO_CONTROL srbControl)
{
    PSENDCMDOUTPARAMS sendCmdOut;
    ULONG requiredSize;

    // Validate buffer size - only need SENDCMDOUTPARAMS for output (no data buffer needed)
    requiredSize = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) - 1;
    if (Srb->DataTransferLength < requiredSize) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: RETURN_STATUS buffer too small (%u < %u)\n",
                       Srb->DataTransferLength, requiredSize);
#endif
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return FALSE;
    }

    sendCmdOut = (PSENDCMDOUTPARAMS)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));

    // Initialize output - no data returned, only status
    memset(sendCmdOut, 0, sizeof(SENDCMDOUTPARAMS));
    sendCmdOut->cBufferSize = 0;
    sendCmdOut->DriverStatus.bDriverError = 0;
    sendCmdOut->DriverStatus.bIDEError = 0;

    // Return SMART status in bReserved bytes (used as output registers)
    // For NVMe, we return PASSING status
    // PASSING: CylLow=0x4F, CylHigh=0xC2
    // FAILING: CylLow=0xF4, CylHigh=0x2C
    //
    // Ideally we'd check NVMe CriticalWarning field, but that requires
    // async Get Log Page. For now, return PASSING.
    // Note: bReserved[0] = CylLow, bReserved[1] = CylHigh (by convention)
    sendCmdOut->DriverStatus.bReserved[0] = SMART_CYL_LOW;   // 0x4F = PASSING
    sendCmdOut->DriverStatus.bReserved[1] = SMART_CYL_HI;    // 0xC2 = PASSING

    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) - 1;
    srbControl->ReturnCode = 0;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: RETURN_STATUS DONE\n");
#endif
    return TRUE;
}

//
// HandleIO_SCSIDISK - Process SMART/ATA pass-through IOCTLs
//
BOOLEAN HandleIO_SCSIDISK(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb)
{
    PSRB_IO_CONTROL srbControl;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: HandleSmartIoctl called - DataTransferLength=%u\n",
                   Srb->DataTransferLength);
#endif

    srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;

    switch (srbControl->ControlCode) {
        case IOCTL_SCSI_PASS_THROUGH:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_PASS_THROUGH\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_MINIPORT:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_GET_INQUIRY_DATA:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_GET_INQUIRY_DATA\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_GET_CAPABILITIES:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_GET_CAPABILITIES\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_PASS_THROUGH_DIRECT:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_PASS_THROUGH_DIRECT\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_GET_ADDRESS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_GET_ADDRESS\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_RESCAN_BUS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_RESCAN_BUS\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_GET_DUMP_POINTERS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_GET_DUMP_POINTERS\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_FREE_DUMP_POINTERS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_FREE_DUMP_POINTERS\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_IDE_PASS_THROUGH:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_IDE_PASS_THROUGH\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_MINIPORT_SMART_VERSION:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_SMART_VERSION\n");
#endif
            return HandleSmartGetVersion(DevExt, Srb, srbControl);
        case IOCTL_SCSI_MINIPORT_IDENTIFY:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_IDENTIFY\n");
#endif
            return HandleSmartIdentify(DevExt, Srb, srbControl);
        case IOCTL_SCSI_MINIPORT_READ_SMART_ATTRIBS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: IOCTL_SCSI_MINIPORT_READ_SMART_ATTRIBS\n");
#endif
            return HandleSmartReadAttribs(DevExt, Srb, srbControl);
        case IOCTL_SCSI_MINIPORT_READ_SMART_THRESHOLDS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_READ_SMART_THRESHOLDS\n");
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
        case IOCTL_SCSI_MINIPORT_ENABLE_SMART:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_ENABLE_SMART\n");
#endif
            return HandleSmartEnableDisable(DevExt, Srb, srbControl, TRUE);

        case IOCTL_SCSI_MINIPORT_DISABLE_SMART:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_DISABLE_SMART\n");
#endif
            return HandleSmartEnableDisable(DevExt, Srb, srbControl, FALSE);
        case IOCTL_SCSI_MINIPORT_RETURN_STATUS:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK IOCTL_SCSI_MINIPORT_RETURN_STATUS\n");
#endif
            return HandleSmartReturnStatus(DevExt, Srb, srbControl);

        default:
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: SCSIDISK unknown IOCTL:%08X function:%03X\n", srbControl->ControlCode, (srbControl->ControlCode >> 2) & 0xFFF);
#endif
            srbControl->ReturnCode = 1;
            return FALSE;
    }
}
