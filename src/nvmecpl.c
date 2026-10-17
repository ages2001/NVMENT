// handling NVME completions
#include "nvment.h"
#include "nvmeutil.h"

//
// NvmeProcessGetLogPageCompletion - Handle Get Log Page command completion
//
VOID NvmeProcessGetLogPageCompletion(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT status, USHORT commandId)
{
    PSCSI_REQUEST_BLOCK Srb;
    PVOID prpBuffer;
    PNVME_SRB_EXTENSION srbExt;
    UCHAR prpPageIndex = 0xFF;

    // Get the untagged SRB from ScsiPort
    // ScsiPort guarantees only one untagged request at a time
    Srb = NvmeGetSrbFromCommandId(DevExt, commandId);

    if (!Srb) {
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: Get Log Page completion - missing Srb!\n");
#endif
        // we can use prpPageIndex to free the log page so we dont leak them
    } else {
        // Get PRP page index from SRB extension
        srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
        prpPageIndex = srbExt->PrpListPage;

#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: GetLogPageCpl - CID=%04X PRP=%u Status=0x%04X\n",
                       commandId, prpPageIndex, status);
#endif
        prpBuffer = GetPrpListPageVirtual(DevExt, prpPageIndex);

        // Determine the request type: LOG SENSE, SAT PASS-THROUGH, or SMART IOCTL
        if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI && Srb->Cdb[0] == SCSIOP_LOG_SENSE) {
            // This is a LOG SENSE completion
            if (status == NVME_SC_SUCCESS && prpBuffer) {
                PNVME_SMART_INFO nvmeSmart = (PNVME_SMART_INFO)prpBuffer;
                UCHAR pageCode = ScsiGetLogPageCodeFromSrb(Srb);
                ULONG bytesWritten = 0;

                // Convert NVMe log page to proper SCSI log page format
                if (NvmeLogPageToScsiLogPage(nvmeSmart, pageCode, Srb->DataBuffer,
                                             Srb->DataTransferLength, &bytesWritten)) {
                    Srb->DataTransferLength = bytesWritten;
                    Srb->SrbStatus = SRB_STATUS_SUCCESS;
                } else {
                    Srb->DataTransferLength = 0;
                    Srb->SrbStatus = SRB_STATUS_ERROR;
                }
            } else {
                Srb->DataTransferLength = 0;
                Srb->SrbStatus = SRB_STATUS_ERROR;
            }
        } else if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI &&
                   (Srb->Cdb[0] == SCSIOP_ATA_PASSTHROUGH16 || Srb->Cdb[0] == SCSIOP_ATA_PASSTHROUGH12)) {
            // This is a SAT ATA PASS-THROUGH completion
            if (status == NVME_SC_SUCCESS && prpBuffer) {
                UCHAR ataCommand;
                UCHAR ataFeatures;
                UCHAR ataCylLow;
                UCHAR ataCylHigh;

                if (!ScsiParseSatCommand(Srb, &ataCommand, &ataFeatures, &ataCylLow, &ataCylHigh)) {
                    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
                } else {
                    // Check which ATA command was requested
                    if (ataCommand == ATA_SMART_CMD && ataFeatures == ATA_SMART_READ_DATA) {
                        // SMART READ DATA - convert NVMe SMART to ATA SMART format
                        // For SAT commands, the data buffer is the raw 512-byte payload.
                        // It does NOT include a SENDCMDOUTPARAMS header.
                        PNVME_SMART_INFO nvmeSmart = (PNVME_SMART_INFO)prpBuffer;
                        PATA_SMART_DATA ataSmart = (PATA_SMART_DATA)Srb->DataBuffer;

                        NvmeSmartToAtaSmart(nvmeSmart, ataSmart);

                        Srb->DataTransferLength = 512;
                        Srb->SrbStatus = SRB_STATUS_SUCCESS;
                    } else {
                        Srb->SrbStatus = SRB_STATUS_ERROR;
                    }
                }
            } else {
                Srb->DataTransferLength = 0;
                Srb->SrbStatus = SRB_STATUS_ERROR;
            }
        } else if (Srb->Function == SRB_FUNCTION_IO_CONTROL) {
            // This is a SMART IOCTL completion
            PSRB_IO_CONTROL srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
            PSENDCMDOUTPARAMS sendCmdOut = (PSENDCMDOUTPARAMS)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));

            if (status != NVME_SC_SUCCESS || !prpBuffer) {
                // Command failed
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: FAILED Log Page completion for SRB_FUNCTION_IO_CONTROL 0x%02X\n", Srb->Function);
#endif
                sendCmdOut->cBufferSize = 0;
                sendCmdOut->DriverStatus.bDriverError = 1;
                sendCmdOut->DriverStatus.bIDEError = 0x04;  // Aborted
                srbControl->ReturnCode = 1;
                Srb->DataTransferLength = 0;
                Srb->SrbStatus = SRB_STATUS_ERROR;
            } else
            if (memcmp(srbControl->Signature, "SCSIDISK", 8) == 0
                && srbControl->ControlCode == IOCTL_SCSI_MINIPORT_READ_SMART_ATTRIBS) {
                    PNVME_SMART_INFO nvmeSmart = (PNVME_SMART_INFO)prpBuffer;
                    PATA_SMART_DATA ataSmart = (PATA_SMART_DATA)sendCmdOut->bBuffer;

                    // Convert NVMe SMART/Health log to ATA SMART format
                    NvmeSmartToAtaSmart(nvmeSmart, ataSmart);

                    sendCmdOut->cBufferSize = 512;
                    memset(&sendCmdOut->DriverStatus, 0, sizeof(DRIVERSTATUS));
                    sendCmdOut->DriverStatus.bDriverError = 0;
                    sendCmdOut->DriverStatus.bIDEError = 0;

                    // Set DataTransferLength to total size returned
                    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL) + sizeof(SENDCMDOUTPARAMS) + 512 - 1;

                    srbControl->ReturnCode = 0;
                    Srb->SrbStatus = SRB_STATUS_SUCCESS;
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Get Log Page completion for SRB_FUNCTION_IO_CONTROL/IOCTL_SCSI_MINIPORT_READ_SMART_ATTRIBS 0x%02X\n", Srb->Function);
#endif
            } else {
                Srb->SrbStatus = SRB_STATUS_ERROR;
            }
        } else {
            // Unknown request type for Get Log Page
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: Get Log Page completion for unknown SRB function 0x%02X\n", Srb->Function);
#endif
            Srb->SrbStatus = SRB_STATUS_ERROR;
        }
        // Complete the SRB, scsiport takes control
        srbExt->PrpListPage = 0xFF;
        ScsiPortNotification(RequestComplete, DevExt, Srb);
        if (DevExt->Busy) {
            DevExt->Busy = FALSE;
            ScsiPortNotification(NextRequest, DevExt, NULL);
        }
    }

    if (prpPageIndex != 0xFF) {
        FreePrpListPage(DevExt, prpPageIndex);
    }
}

//
// NvmeProcessUserExtensionCompletion - Handle userspace called NVMe extension completion
//
VOID NvmeProcessUserExtensionCompletion(
    IN PHW_DEVICE_EXTENSION DevExt,
    IN USHORT commandId,
    IN USHORT status,
    IN PNVME_COMPLETION cqEntry)
{
    PSCSI_REQUEST_BLOCK Srb;
    PNVME_SRB_EXTENSION srbExt;
    PVOID prpBuffer;
    PNVME_PASS_THROUGH nvmePassThru;
    ULONG copySize;
    ULONG dataOffset;

#ifdef NVMENT_DBG
    ScsiDebugPrint(0, "NVMENT: User extension completion - CID=%04X Status=0x%04X DW0=%08X DW1=%08X\n",
                   commandId, status, cqEntry->DW0, cqEntry->DW1);
#endif

    // Retrieve SRB using ScsiPortGetSrb (untagged because SCSI doesn't use tags here)
    // If that fails, use NonTaggedInFlight as backup
    Srb = NvmeGetSrbFromCommandId(DevExt, commandId);

    if (!Srb) {
        return;
    }

    // Get PRP buffer pointer
    srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;
    if (srbExt && srbExt->PrpListPage != 0xFF) {
        prpBuffer = GetPrpListPageVirtual(DevExt, srbExt->PrpListPage);
    } else {
        prpBuffer = NULL;
    }

    if (status == NVME_SC_SUCCESS && prpBuffer) {
        // Handle different buffer layouts based on SRB function
        if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI) {
            // Direct SCSI command (0xB5) - simple buffer layout
            // Data is at offset 0 in DataBuffer
            dataOffset = 0;
            copySize = NVME_PAGE_SIZE;

            if (Srb->DataTransferLength >= NVME_PAGE_SIZE) {
                memcpy(Srb->DataBuffer, prpBuffer, copySize);
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: Direct SCSI 0xB5 completed - copied %u bytes\n", copySize);
#endif
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
            } else {
                copySize = 0;
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: ERROR - Direct SCSI buffer too small! DataTransferLength=%u\n",
                               Srb->DataTransferLength);
#endif
                Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
            }

        } else if (Srb->Function == SRB_FUNCTION_IO_CONTROL) {
            PSRB_IO_CONTROL srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
            // IO_CONTROL path (NvmeMini) - complex buffer layout
            // Buffer layout: SRB_IO_CONTROL + NVME_PASS_THROUGH + data
            //
            // srbControl->Length = total payload after SRB_IO_CONTROL
            // The 4KB NVMe data is at the END of the payload
            // So: dataOffset = sizeof(SRB_IO_CONTROL) + (Length - 4096)
            nvmePassThru = (PNVME_PASS_THROUGH)((PUCHAR)Srb->DataBuffer + sizeof(SRB_IO_CONTROL));
            dataOffset = sizeof(SRB_IO_CONTROL) + sizeof(NVME_PASS_THROUGH);

            // Populate the Completion array (4 DWORDs from completion queue entry)
            nvmePassThru->Completion[0] = cqEntry->DW0;
            nvmePassThru->Completion[1] = cqEntry->DW1;
            nvmePassThru->Completion[2] = ((ULONG)cqEntry->SQID << 16) | cqEntry->SQHead;
            nvmePassThru->Completion[3] = ((ULONG)cqEntry->Status << 16) | cqEntry->CID;

            // Verify we have enough space
            if (Srb->DataTransferLength >= dataOffset + NVME_PAGE_SIZE) {
                copySize = NVME_PAGE_SIZE;
                memcpy((PUCHAR)Srb->DataBuffer + dataOffset, prpBuffer, copySize);
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: NvmeMini IO_CONTROL completed - copied %u bytes to offset %u (TotalLen=%u, Length=%u)\n",
                               copySize, dataOffset, Srb->DataTransferLength, srbControl->Length);
                ScsiDebugPrint(0, "NVMENT: Completion: [%08X %08X %08X %08X]\n",
                               nvmePassThru->Completion[0], nvmePassThru->Completion[1],
                               nvmePassThru->Completion[2], nvmePassThru->Completion[3]);
#endif
                // DataTransferLength stays the same (total buffer size from userland)
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
                srbControl->ReturnCode = 0;  // success
            } else {
                // Buffer too small, shouldn't happen
                copySize = 0;
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: ERROR - NvmeMini buffer too small! DataTransferLength=%u dataOffset=%u\n",
                               Srb->DataTransferLength, dataOffset);
#endif
                // DataTransferLength stays the same (total buffer size from userland)
                Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
                srbControl->ReturnCode = 5;  // insufficient out buffer
            }

        }
    } else {
        // On error, don't modify the buffer
        Srb->SrbStatus = SRB_STATUS_ERROR;
        if (Srb->Function == SRB_FUNCTION_IO_CONTROL) {
            PSRB_IO_CONTROL srbControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
            srbControl->ReturnCode = 1;  // Error
        }
#ifdef NVMENT_DBG
        ScsiDebugPrint(0, "NVMENT: user extension command failed with NVMe status 0x%04X\n", status);
#endif
    }

    // Free PRP list page if allocated
    if (srbExt && srbExt->PrpListPage != 0xFF) {
        FreePrpListPage(DevExt, srbExt->PrpListPage);
        srbExt->PrpListPage = 0xFF;
    }

    // Complete the request
    ScsiPortNotification(RequestComplete, DevExt, Srb);
    ScsiPortNotification(NextRequest, DevExt, NULL);
}

//
// NvmeProcessAdminCompletion - Process admin queue completions
//
BOOLEAN NvmeProcessAdminCompletion(IN PHW_DEVICE_EXTENSION DevExt)
{
    PNVME_QUEUE Queue = &DevExt->AdminQueue;
    PNVME_COMPLETION cqEntry;
    BOOLEAN processed = FALSE;
    USHORT status;
    USHORT commandId;
    ULONG queueIndex;
    ULONG expectedPhase;

    if (Queue->CompletionQueue == NULL) {
        return FALSE;
    }

    while (TRUE) {
        // Calculate queue index from current head
        queueIndex = Queue->CompletionQueueHead & Queue->QueueSizeMask;

        // Calculate expected phase from counter (toggles every QueueSize completions)
        expectedPhase = (Queue->CompletionQueueHead >> Queue->QueueSizeBits) & 1;

        // Get completion queue entry
        cqEntry = (PNVME_COMPLETION)((PUCHAR)Queue->CompletionQueue +
                                     (queueIndex * NVME_CQ_ENTRY_SIZE));

        // Check phase bit
        if ((cqEntry->Status & 1u) != expectedPhase) {
            // No more completions
            break;
        }

        processed = TRUE;

        // Extract status (SC, SCT, DNR) and command ID
        status = (USHORT)((cqEntry->Status >> 1) & 0x7FFF);
        commandId = cqEntry->CID;

        // Update submission queue head from completion entry
        Queue->SubmissionQueueHead = cqEntry->SQHead;

        // Increment completion queue head
        Queue->CompletionQueueHead++;

#ifdef NVMENT_DBG_CMD
        ScsiDebugPrint(0, "NVMENT: NvmeProcessAdminCompletion - CID=%04X Status=0x%04X SQHead=%d\n",
                       commandId, status, Queue->SubmissionQueueHead);
#endif
        if (commandId == ADMIN_CID_SHUTDOWN_DELETE_SQ || commandId == ADMIN_CID_SHUTDOWN_DELETE_CQ) {
#ifdef NVMENT_DBG
            if (status != NVME_SC_SUCCESS) {
                ScsiDebugPrint(0, "NVMENT: SHUTDOWN_DELETE_%s failed with status 0x%04X\n",
                               commandId == ADMIN_CID_SHUTDOWN_DELETE_SQ ? "SQ" : "CQ", status);
            }
#endif
            continue;
        }

        switch (CID_KIND(commandId)) {
            case ADMIN_KIND_SYNC:
                DevExt->SyncStatus = status;
                DevExt->SyncDw0 = cqEntry->DW0;
                DevExt->SyncDone = TRUE;
                break;

            case ADMIN_KIND_GET_LOG_PAGE:
                NvmeProcessGetLogPageCompletion(DevExt, status, commandId);
                break;

            case ADMIN_KIND_USER_IDENTIFY:
            case ADMIN_KIND_USER_GET_LOG_PAGE:
                NvmeProcessUserExtensionCompletion(DevExt, commandId, status, cqEntry);
                break;

            default:
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: unknown admin CID %04X\n", commandId);
#endif
                break;
        }
    }

    // Ring completion doorbell ONCE with the final head position after processing all completions
    // This acknowledges all processed completions and clears the interrupt
    if (processed) {
        NvmeRingDoorbell(DevExt, Queue->QueueId, FALSE, (USHORT)(Queue->CompletionQueueHead & Queue->QueueSizeMask));
    }

    return processed;
}

//
// NvmeProcessIoCompletion - Process I/O queue completions
//
BOOLEAN NvmeProcessIoCompletion(IN PHW_DEVICE_EXTENSION DevExt)
{
    PNVME_QUEUE Queue = &DevExt->IoQueue;
    PNVME_COMPLETION cqEntry;
    BOOLEAN processed = FALSE;
    USHORT status;
    USHORT commandId;
    PSCSI_REQUEST_BLOCK Srb;
    ULONG queueIndex;
    ULONG expectedPhase;

#ifdef NVMENT_DBG_EXTRA
    if (DevExt->TotalRequests) {
        ScsiDebugPrint(0, "NVMENT: NvmeProcessIoCompletion called\n");
    }
#endif

    if (Queue->CompletionQueue == NULL) {
        return FALSE;
    }

    while (TRUE) {
        // Calculate queue index from current head
        queueIndex = Queue->CompletionQueueHead & Queue->QueueSizeMask;

        // Calculate expected phase from counter (toggles every QueueSize completions)
        expectedPhase = (Queue->CompletionQueueHead >> Queue->QueueSizeBits) & 1;

        // Get completion queue entry
        cqEntry = (PNVME_COMPLETION)((PUCHAR)Queue->CompletionQueue +
                                     (queueIndex * NVME_CQ_ENTRY_SIZE));

        // Check phase bit
        if ((cqEntry->Status & 1u) != expectedPhase) {
            // No more completions
            break;
        }

        processed = TRUE;

#ifdef NVMENT_DBG_EXTRA
        ScsiDebugPrint(0, "NVMENT: I/O completion found! QueueIdx=%d Phase=%d\n", queueIndex, expectedPhase);
#endif

        // Extract status (SC, SCT, DNR) and command ID
        status = (USHORT)((cqEntry->Status >> 1) & 0x7FFF);
        commandId = cqEntry->CID;

        // Update submission queue head from completion entry
        Queue->SubmissionQueueHead = cqEntry->SQHead;

        // Increment completion queue head
        Queue->CompletionQueueHead++;

#ifdef NVMENT_DBG_CMD
        ScsiDebugPrint(0, "NVMENT: NvmeProcessIoCompletion - CID=%d Status=0x%04X SQHead=%d\n",
                       commandId, status, Queue->SubmissionQueueHead);
#endif
        // Retrieve SRB from command ID using ScsiPortGetSrb
        Srb = NvmeGetSrbFromCommandId(DevExt, commandId);

        // An ORDERED flush has no SRB of its own
        if (commandId & CID_ORDERED_FLUSH_FLAG) {
            if (DevExt->CurrentQueueDepth > 0) {
                DevExt->CurrentQueueDepth--;
            }
            continue;
        }

        if (Srb == NULL) {
#ifdef NVMENT_DBG
            ScsiDebugPrint(0, "NVMENT: ERROR - Got NULL SRB for CID=%04X! This should not happen.\n", commandId);
#endif
            // Clear non-tagged flag to prevent driver from getting stuck
            if (commandId & CID_NON_TAGGED_FLAG) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: Clearing NonTaggedInFlight flag for orphaned CID\n");
#endif
            }
        } else {
            PNVME_SRB_EXTENSION srbExt;

            // Validate SRB before processing
            if (Srb->SrbStatus != SRB_STATUS_PENDING) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: ERROR - SRB CID=%d has invalid status 0x%02X (expected PENDING=0x%02X)\n",
                               commandId, Srb->SrbStatus, SRB_STATUS_PENDING);
                ScsiDebugPrint(0, "NVMENT:        This suggests DOUBLE COMPLETION! SRB=%p\n", Srb);
#endif
                // Skip this - it's already been completed, continue to next completion
                continue;
            }

            if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI && Srb->Function != SRB_FUNCTION_FLUSH) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: ERROR - SRB CID=%d has invalid function 0x%02X\n",
                               commandId, Srb->Function);
#endif
            }

            if (!Srb->SrbExtension) {
#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: ERROR - SRB CID=%d has NULL SrbExtension!\n", commandId);
#endif
                // Skip this completion, continue to next completion
                continue;
            }

            // Get SRB extension for PRP list cleanup
            srbExt = (PNVME_SRB_EXTENSION)Srb->SrbExtension;

            // Free PRP list page if allocated
            if (srbExt->PrpListPage != 0xFF) {
                FreePrpListPage(DevExt, srbExt->PrpListPage);
                srbExt->PrpListPage = 0xFF;
            }

            // Check if this was a TRIM operation that we need to restore buffer for
            if (DevExt->TrimEnable && DevExt->TrimPattern != NULL && Srb->DataTransferLength >= 4096) {
                PCDB cdb = (PCDB)Srb->Cdb;
                BOOLEAN isWrite = FALSE;

                // Check if this is a write operation
                switch (cdb->CDB10.OperationCode) {
                    case SCSIOP_WRITE6:
                    case SCSIOP_WRITE:
                        isWrite = TRUE;
                        break;
                }

                // If write, check if bytes 16-4095 match TrimPattern (excluding first 16 bytes we corrupted)
                if (isWrite && Srb->DataBuffer) {
                    PUCHAR dataBuffer = (PUCHAR)Srb->DataBuffer;
                    // Compare bytes 16-4095 with TrimPattern offset by 4 ULONGs (16 bytes)
                    if (memcmp(dataBuffer + 16, (PUCHAR)DevExt->TrimPattern + 16, 4096 - 16) == 0) {
                        // This was a TRIM operation - restore the first 16 bytes from TrimPattern
#ifdef NVMENT_DBG_EXTRA
                        ScsiDebugPrint(0, "NVMENT: Restoring first 16 bytes of TRIM buffer\n");
#endif
                        memcpy(dataBuffer, DevExt->TrimPattern, 16);
                    }
                }
            }

            // Set SRB status based on NVMe status
            if (status == NVME_SC_SUCCESS) {
                Srb->SrbStatus = SRB_STATUS_SUCCESS;
#ifdef NVME_DBG_EXTRA
                // to spammy enable by default
                ScsiDebugPrint(0, "NVMENT: Completing CID=%d SRB=%p SUCCESS\n", commandId, Srb);
#endif
            } else {
                // Command failed - auto-sense data from the NVMe status
                NvmeSetSenseFromStatus(DevExt, Srb, status);

#ifdef NVMENT_DBG
                ScsiDebugPrint(0, "NVMENT: I/O command failed - CID=%d NVMe Status=0x%02X\n",
                               commandId, status);
#endif
            }

            // Complete the request - ScsiPort takes ownership of the SRB
            ScsiPortNotification(RequestComplete, DevExt, Srb);
            if (DevExt->Busy) {
                // hopefully some resources freed up so signal that we can process next request
                DevExt->Busy = FALSE;
                ScsiPortNotification(NextRequest, DevExt, NULL);
            }


            // Decrement queue depth tracking
            if (DevExt->CurrentQueueDepth > 0) {
                DevExt->CurrentQueueDepth--;
            }
        }
    }

    // Ring completion doorbell ONCE with the final head position after processing all completions
    // This acknowledges all processed completions and clears the interrupt
    if (processed) {
        NvmeRingDoorbell(DevExt, Queue->QueueId, FALSE, (USHORT)(Queue->CompletionQueueHead & Queue->QueueSizeMask));
    }

    return processed;
}
