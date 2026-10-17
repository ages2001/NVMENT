//
// NVMe Driver for Windows 2000 - Header File
// Contains all defines, constants, and type definitions
//

#ifndef _NVMENT_H_
#define _NVMENT_H_

#include <miniport.h>
#include <scsi.h>
#include <devioctl.h>
#include <ntddscsi.h>
#include <ntdddisk.h>
#include "nvme.h"
#include "scsiext.h"

//#define NVMENT_DBG
// extra spammy logging for NVMe commands
//#define NVMENT_DBG_CMD
//#define NVMENT_DBG_EXTRA
// uncomment to enable debugging and logging
/* for debug messages use
 * ScsiDebugPrint(0, "NVMENT: ...\n");
 * it seems only level 0 messages are displayed, possibly registry changes are needed
 * #define NVMENT_DBG_STATS
 */

//
// We only use 4K pages even if drive supports more
//
#define NVME_PAGE_SIZE                      0x1000  // 4KB page size
#define NVME_PAGE_MASK                      (NVME_PAGE_SIZE-1)
#define NVME_PAGE_SHIFT                     12      // log2(NVME_PAGE_SIZE)

//
// NVMe PCI Class Codes
//
#define PCI_CLASS_MASS_STORAGE_CONTROLLER   0x01
#define PCI_SUBCLASS_NON_VOLATILE_MEMORY    0x08
#define PCI_PROGIF_NVME                     0x02

//
// PCI Configuration Space Offsets
//
#define PCI_VENDOR_ID_OFFSET                0x00
#define PCI_DEVICE_ID_OFFSET                0x02
#define PCI_COMMAND_OFFSET                  0x04
#define PCI_STATUS_OFFSET                   0x06
#define PCI_REVISION_ID_OFFSET              0x08
#define PCI_CLASS_CODE_OFFSET               0x09
#define PCI_HEADER_TYPE_OFFSET              0x0E
#define PCI_BASE_ADDRESS_0                  0x10
#define PCI_BASE_ADDRESS_1                  0x14
#define PCI_SUBSYSTEM_VENDOR_ID_OFFSET      0x2C
#define PCI_SUBSYSTEM_ID_OFFSET             0x2E
#define PCI_INTERRUPT_LINE_OFFSET           0x3C
#define PCI_INTERRUPT_PIN_OFFSET            0x3D

//
// PCI Command Register bits
//
#define PCI_ENABLE_IO_SPACE                 0x0001
#define PCI_ENABLE_MEMORY_SPACE             0x0002
#define PCI_ENABLE_BUS_MASTER               0x0004
#define PCI_INTERRUPT_DISABLE               0x0400  // Bit 10: Disable INTx interrupt assertion

//
// Doorbell registers (stride determined by CAP.DSTRD)
// (not defined in nvme.h as it's driver-specific)
//
#define NVME_REG_DBS        0x1000  // Doorbell base

//
// Windows NT 3.1 support in the NT4 (x86) build: NT 3.1's ScsiPort takes only
// its own 40h-byte HW_INITIALIZATION_DATA, has a 58h-byte ConfigInfo without
// SlotNumber/MaximumNumberOfTargets, its HAL has no PCI configuration access
// and ScsiPortSetBusDataByOffset/ScsiPortValidateRange do not exist.
//
#if defined(_X86_) && (_WIN32_WINNT < 0x500) && !defined(NVMENT_W9X)
#define NVMENT_NT31_COMPAT
#endif

//
// Windows 95/98/Me (NVMENT_W9X, nvment9x.mpd): the NT 4.0 interface of
// SCSIPORT.PDR. Registers are mapped with the VMM's _MapPhysToLinear,
// requests go one at a time per LU, and completions are polled (Fast
// Polling) unless "IRQ=1" is given.
//
#ifdef NVMENT_W9X
#if (_WIN32_WINNT >= 0x500)
#error NVMENT_W9X is the NT 4.0 interface
#endif
PVOID __cdecl NvmeMapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG Flags);
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field)       ((LONG)&(((type *)0)->field))
#endif

#ifdef NVMENT_NT31_COMPAT
#define NVMENT_HWINIT_SIZE_NT31         ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, VendorId))
#define NVMENT_HWINIT_SIZE_NT35         ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, DeviceId) + sizeof(PVOID))
// ConfigInfo field present in the structure ScsiPort handed us?
#define NVMENT_CONFIG_HAS(ci, field)    ((ci)->Length >= (ULONG)FIELD_OFFSET(PORT_CONFIGURATION_INFORMATION, field) + sizeof((ci)->field))
#endif

//
// Namespaces: each active namespace (up to NVMENT_MAX_LUNS) is one LUN of target 0
//
#define NVMENT_MAX_LUNS                 8
#define NVMENT_MAX_SG_PAGES             32      // PRP list page pool
#define NVMENT_MAX_CONTROLLERS          8

//
// Timeouts
//
#define NVMENT_ADMIN_TIMEOUT_MS         5000    // synchronous admin commands
#define NVMENT_POLL_INTERVAL_USEC       10      // Fast Polling
#define NVMENT_WATCHDOG_USEC            100000  // interrupt watchdog while commands are outstanding
#define NVMENT_APST_MAX_LATENCY_US      100000  // APST: deepest state with an exit latency up to 100 ms
#define NVMENT_MISSED_IRQ_LIMIT         5       // watchdog finds lost interrupts this often -> Fast Polling

//
// Queue sizes and scatter-gather limits
//
#define NVME_MAX_QUEUE_SIZE     (NVME_PAGE_SIZE/NVME_SQ_ENTRY_SIZE)  // Maximum we can fit in a page (64)
                                                                 // Actual size determined by min(NVME_MAX_QUEUE_SIZE, MQES+1)
//
// NVMe Queue Pair
//
typedef struct _NVME_QUEUE {
    PVOID SubmissionQueue;
    PVOID CompletionQueue;
    PHYSICAL_ADDRESS SubmissionQueuePhys;
    PHYSICAL_ADDRESS CompletionQueuePhys;
    ULONG SubmissionQueueHead;   // Protected by SubmissionLock
    ULONG SubmissionQueueTail;   // Protected by SubmissionLock
    ULONG CompletionQueueHead;   // Monotonic counter, never wraps. Phase = (head >> QueueSizeBits) & 1. Protected by CompletionLock
    ULONG CompletionQueueTail;
    USHORT QueueSizeMask;        // QueueSize - 1, for index masking
    USHORT QueueId;
    USHORT QueueSize;
    UCHAR QueueSizeBits;         // log2(QueueSize), for phase calculation
    UCHAR Reserved;              // Padding for alignment
    ULONG reserved1;
    ULONG reserved2;
} NVME_QUEUE, *PNVME_QUEUE;

// Command ID encoding
// Bit 15: Set for non-tagged request
// Bit 14: Set for ORDERED flush (used with bit 15 clear)
// Bits 11-13: admin request kind (admin queue only, 0 on the I/O queue)
// Bits 8-10: LUN (namespace index)
// Bits 0-7: QueueTag (tagged) or sequence number (non-tagged)
#define CID_NON_TAGGED_FLAG 0x8000
#define CID_ORDERED_FLUSH_FLAG 0x4000
#define CID_KIND_SHIFT      11
#define CID_KIND_MASK       0x3800
#define CID_LUN_SHIFT       8
#define CID_LUN_MASK        0x0700
#define CID_TAG_MASK        0x00FF
#define CID_VALUE_MASK      0x3FFF
#define CID_LUN(cid)        ((UCHAR)(((cid) & CID_LUN_MASK) >> CID_LUN_SHIFT))
#define CID_KIND(cid)       (((cid) & CID_KIND_MASK) >> CID_KIND_SHIFT)

//
// SRB Extension - per-request data stored by ScsiPort
//
typedef struct _NVME_SRB_EXTENSION {
    UCHAR PrpListPage;              // Which PRP list page is allocated (0xFF if none)
    UCHAR Reserved[3];              // Padding for alignment
} NVME_SRB_EXTENSION, *PNVME_SRB_EXTENSION;

//
// Admin command kinds (CID bits 11-13). SRB-carrying admin commands use
// CID = kind | LUN | tag-or-sequence (see NvmeBuildAdminCommandId).
//
#define ADMIN_KIND_SYNC                 1   // synchronous command, polled by NvmeAdminSync
#define ADMIN_KIND_GET_LOG_PAGE         2   // Get Log Page for LOG SENSE / SAT / SMART IOCTL
#define ADMIN_KIND_USER_IDENTIFY        3   // IDENTIFY from userspace via NvmeMini
#define ADMIN_KIND_USER_GET_LOG_PAGE    4   // GET_LOG_PAGE same
#define ADMIN_CID_SYNC                  (ADMIN_KIND_SYNC << CID_KIND_SHIFT)

//
// Admin Command IDs for shutdown sequence (special, non-colliding values)
//
#define ADMIN_CID_SHUTDOWN_DELETE_SQ    0xFFFE
#define ADMIN_CID_SHUTDOWN_DELETE_CQ    0xFFFD

//
// Per-LUN (namespace) data
//
typedef struct _NVME_LUN {
    ULONGLONG SizeInBlocks;                         // NSZE
    ULONG NamespaceId;                              // 0 = no namespace on this LUN
    ULONG BlockSize;                                // from the LBA format in use
    PSCSI_REQUEST_BLOCK NonTaggedInFlight;          // ScsiPort sends one untagged request per LU
    BOOLEAN Thin;                                   // NSFEAT thin provisioning
    UCHAR BlockShift;                               // log2(BlockSize)
    BOOLEAN SensePending;                           // sense kept for REQUEST SENSE (no autosense)
    UCHAR Reserved;
    UCHAR Sense[18];                                // fixed format
    UCHAR Reserved2[2];
} NVME_LUN, *PNVME_LUN;

//
// "NVMENT" IOCTL control codes (SRB_IO_CONTROL.ControlCode)
//
#define NVMENT_IOCTL_QUERY_INFO       0x1000
#define NVMENT_IOCTL_TRIM_MODE_ON     0x1001
#define NVMENT_IOCTL_TRIM_MODE_OFF    0x1002
#define NVMENT_IOCTL_POWER_DOWN       0x1010  // shutdown notification; the next request or POWER_UP brings it back
#define NVMENT_IOCTL_POWER_UP         0x1011

#define NVMENT_INFO_POLLING             0x00000001  // Fast Polling (no interrupt in use)
#define NVMENT_INFO_POWERED_DOWN        0x00000002
#define NVMENT_INFO_VWC                 0x00000004  // volatile write cache enabled
#define NVMENT_INFO_TRIM_MODE           0x00000008  // writes of the TRIM pattern become deallocates
#define NVMENT_INFO_APST                0x00000010  // autonomous power state transitions on (APST=1)

typedef struct _NVMENT_INFO {
    ULONG Version;                                  // NVMENT_DRIVER_VERSION
    ULONG NvmeVersion;                              // controller VS register
    ULONG Flags;                                    // NVMENT_INFO_xxx
    ULONG LunCount;
    ULONG PowerState;                               // current NVMe power state
    ULONG MaxTransfer;                              // bytes
    ULONG QueueDepthMax;
    ULONG InterruptCount;
    ULONG NamespaceId[NVMENT_MAX_LUNS];
    ULONG BlockSize[NVMENT_MAX_LUNS];
    ULONGLONG SizeInBlocks[NVMENT_MAX_LUNS];
} NVMENT_INFO, *PNVMENT_INFO;

#define NVMENT_DRIVER_VERSION   0x03000000  // 3.0.0.0

//
// Device extension structure - stores per-adapter data
//
typedef struct _HW_DEVICE_EXTENSION {
    ULONG AdapterIndex;                             // Offset 0x00 (0)
    PVOID MappedAddress;                            // Offset 0x04 (4)
    ULONG IoPortBase;                               // Offset 0x08 (8)
    ULONG BusNumber;                                // Offset 0x0C (12)
    ULONG SlotNumber;                               // Offset 0x10 (16)
    USHORT VendorId;                                // Offset 0x14 (20)
    USHORT DeviceId;                                // Offset 0x16 (22)
    USHORT SubsystemVendorId;                       // Offset 0x18 (24)
    USHORT SubsystemId;                             // Offset 0x1A (26)
    UCHAR RevisionId;                               // Offset 0x1C (28)
    UCHAR Reserved2[3];                             // Offset 0x1D (29)
    PVOID ControllerRegisters;                      // Offset 0x20 (32)
    ULONG ControllerRegistersLength;                // Offset 0x24 (36)

    // NVMe specific fields
    ULONGLONG ControllerCapabilities;               // Offset 0x28 (40) [8-byte aligned]
    ULONG Version;                                  // Offset 0x30 (48)
    ULONG Reserved1;                                // Offset 0x34 (52)
    ULONG DoorbellStride;                           // Offset 0x38 (56)
    USHORT MaxQueueEntries;                         // Offset 0x3C (60)
    USHORT SgListPages;                             // Offset 0x3E (62)

    // Admin Queue
    NVME_QUEUE AdminQueue;                          // Offset 0x40 (64) - 56 bytes [8-byte aligned]

    // I/O Queue (single queue for simplicity)
    NVME_QUEUE IoQueue;                             // Offset 0x78 (120) - 56 bytes [8-byte aligned]

    // Command tracking
    PSCSI_REQUEST_BLOCK NonTaggedInFlight;          // unused, per LUN (NVME_LUN)
    USHORT NextNonTaggedId;                         // Offset 0xB4 (180)
    BOOLEAN Busy;                                   // Offset 0xB6 (182)
    BOOLEAN InitComplete;                           // Offset 0xB7 (183)

    ULONG Reserved4a;                               // Offset 0xB8 (184)
    ULONG InterruptCount;                           // Offset 0xBC (188)

    // PRP list pages for scatter-gather (shared pool, allocated after init)
    // Note: During init, UtilityBuffer points to the same memory
    PHYSICAL_ADDRESS PrpListPagesPhys;              // Offset 0xC0 (192) [8-byte aligned]
    PHYSICAL_ADDRESS PrpPagePhys[NVMENT_MAX_SG_PAGES]; // every pool page (not contiguous on 9x)
    PVOID PrpListPages;                             // Offset 0xC8 (200)
    ULONG PrpListPageBitmap;                        // Offset 0xCC (204)

    // Statistics (current and maximum)
    ULONG CurrentQueueDepth;                        // Offset 0xD0 (208)
    ULONG MaxQueueDepthReached;                     // Offset 0xD4 (212)
    ULONG CurrentPrpListPagesUsed;                  // Offset 0xD8 (216)
    ULONG MaxPrpListPagesUsed;                      // Offset 0xDC (220)

    // I/O statistics
    ULONGLONG TotalBytesRead;                       // Offset 0xE0 (224) [8-byte aligned]
    ULONGLONG TotalBytesWritten;                    // Offset 0xE8 (232) [8-byte aligned]
    ULONG TotalRequests;                            // Offset 0xF0 (240)
    ULONG TotalReads;                               // Offset 0xF4 (244)
    ULONG TotalWrites;                              // Offset 0xF8 (248)
    ULONG MaxReadSize;                              // Offset 0xFC (252)
    ULONG MaxWriteSize;                             // Offset 0x100 (256)
    ULONG RejectedRequests;                         // Offset 0x104 (260)

    // Utility buffer (4KB, used during init, then aliased as PRP list pages)
    PHYSICAL_ADDRESS UtilityBufferPhys;             // Offset 0x108 (264) [8-byte aligned]
    PVOID UtilityBuffer;                            // Offset 0x110 (272)

    // Controller information
    ULONG NumberOfNamespaces;                       // Offset 0x114 (276)
    UCHAR ControllerSerialNumber[21];               // Offset 0x118 (280)
    UCHAR ControllerModelNumber[41];                // Offset 0x12D (301)
    UCHAR ControllerFirmwareRevision[9];            // Offset 0x156 (342)
    BOOLEAN SMARTEnabled;                           // Offset 0x15F (351)
    UCHAR MaxDataTransferSizePower;                 // Offset 0x160 (352) - MDTS from controller (power of 2)
    UCHAR Reserved5_1[3];                           // Offset 0x161 (353) - alignment
    ULONG MaxTransferSizeBytes;                     // Offset 0x164 (356) - Computed max transfer in bytes

    // Namespace information
    ULONGLONG NamespaceSizeInBlocks;                // Offset 0x168 (360) - unused, per LUN (NVME_LUN)
    ULONG NamespaceBlockSize;                       // Offset 0x170 (368) - unused, per LUN (NVME_LUN)
    ULONG UncachedExtensionOffset;                  // Offset 0x174 (372)

    // Uncached memory allocation
    PHYSICAL_ADDRESS UncachedExtensionPhys;         // Offset 0x178 (376) [8-byte aligned]
    PVOID UncachedExtensionBase;                    // Offset 0x180 (384)
    ULONG UncachedExtensionSize;                    // Offset 0x184 (388)
    ULONG FallbackTimerNeeded;                      // Offset 0x188 (392) - unused (NvmeWatchdog)

    // TRIM mode support
    BOOLEAN TrimEnable;
    UCHAR Reserved5[3];
    PULONG TrimPattern;                             // 4KB pattern buffer (uncached extension)

    // Namespaces (LUN n = Luns[n])
    NVME_LUN Luns[NVMENT_MAX_LUNS];
    ULONG LunCount;

    // Controller features (Identify Controller)
    USHORT Oncs;                                    // Optional NVM Command Support
    UCHAR Npss;                                     // highest power state number
    UCHAR IdlePowerState;                           // deepest non-operational state, 0 if none
    BOOLEAN VwcPresent;                             // volatile write cache present
    BOOLEAN VwcEnabled;                             // ... and enabled (Get Features)
    UCHAR PowerState;                               // power state we last set
    UCHAR Reserved6;
    BOOLEAN ApstAllowed;                            // APST=1: autonomous power state transitions
    BOOLEAN ApstSupported;                          // Identify Controller APSTA
    BOOLEAN ApstOn;                                 // enabled on the controller
    UCHAR Reserved6a;
    ULONG ApstTable[64];                            // 32 entries of 8 bytes, Set Features 0Ch data

    // Mode of operation
    BOOLEAN PollMode;                               // Fast Polling: no interrupt in use
    BOOLEAN DumpMode;                               // crash dump / hibernation stack
    BOOLEAN Stopped;                                // shut down (power down); set up again on the next request
    BOOLEAN TimerArmed;                             // interrupt watchdog armed
    ULONG MissedInterrupts;                         // watchdog found completions no interrupt reported
    BOOLEAN ResourcesAllocated;                     // queues and PRP pool carved out of the uncached extension
    BOOLEAN RequestsFlushed;                        // NvmeReinitialize returned outstanding requests
    BOOLEAN SyncDone;                               // NvmeAdminSync completion
    UCHAR Reserved7;
    USHORT SyncStatus;                              // ... its status (Status >> 1)
    ULONG SyncDw0;                                  // ... and DW0

} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

//
// Forward declarations of miniport entry points
//
ULONG DriverEntry(IN PVOID DriverObject, IN PVOID Argument2);
ULONG HwFindAdapter(IN PVOID DeviceExtension, IN PVOID HwContext,
                    IN PVOID BusInformation, IN PCHAR ArgumentString,
                    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                    OUT PBOOLEAN Again);
BOOLEAN HwInitialize(IN PVOID DeviceExtension);
BOOLEAN HwStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN HwInterrupt(IN PVOID DeviceExtension);
BOOLEAN HwResetBus(IN PVOID DeviceExtension, IN ULONG PathId);
#if (_WIN32_WINNT >= 0x500)
SCSI_ADAPTER_CONTROL_STATUS HwAdapterControl(IN PVOID DeviceExtension,
                                              IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                              IN PVOID Parameters);
#else
BOOLEAN HwAdapterState(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN BOOLEAN SaveState);
#endif
//
// Helper functions
//
BOOLEAN IsNvmeDevice(UCHAR BaseClass, UCHAR SubClass, UCHAR ProgIf);
UCHAR ReadPciConfigByte(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset);
USHORT ReadPciConfigWord(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset);
ULONG ReadPciConfigDword(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset);
VOID WritePciConfigWord(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN USHORT Value);
VOID WritePciConfigDword(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN ULONG Value);
ULONG GetPciConfig(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Bus, IN ULONG Slot, OUT PVOID Buffer, IN ULONG Length);
VOID PciProbeAccess(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN ArgPresent(IN PCHAR Args, IN PCHAR Key);
BOOLEAN NvmeIsOwnSignature(IN PUCHAR Signature);

//
// NVMe helper functions
//
ULONG NvmeReadReg32(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset);
VOID NvmeWriteReg32(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN ULONG Value);
ULONGLONG NvmeReadReg64(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset);
VOID NvmeWriteReg64(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Offset, IN ULONGLONG Value);
BOOLEAN NvmeWaitForReady(IN PHW_DEVICE_EXTENSION DevExt, IN BOOLEAN WaitForReady);
BOOLEAN NvmeAdminSync(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd, IN ULONG TimeoutMs);
BOOLEAN NvmeStartController(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeReinitialize(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeCheckState(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeSetPowerState(IN PHW_DEVICE_EXTENSION DevExt, IN UCHAR PowerState);
BOOLEAN NvmeSetVolatileWriteCache(IN PHW_DEVICE_EXTENSION DevExt, IN BOOLEAN Enable);
VOID NvmeArmWatchdog(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmeEnterPollMode(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmePollSrb(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
USHORT NvmeBuildAdminCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Kind);
VOID NvmeSetSenseFromStatus(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN USHORT Status);
VOID ScsiSetSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR SenseKey, IN UCHAR Asc, IN UCHAR Ascq);
BOOLEAN ScsiGetLbaCount(IN PSCSI_REQUEST_BLOCK Srb, OUT PULONGLONG Lba, OUT PULONG Count, OUT PBOOLEAN IsWrite, OUT PBOOLEAN Fua);
BOOLEAN NvmeSubmitIoCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd);
BOOLEAN NvmeSubmitAdminCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PNVME_COMMAND Cmd);
BOOLEAN NvmeProcessAdminCompletion(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmeShutdownController(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmePowerDown(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeSanitizeController(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeInitializeController(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmeEnableInterrupts(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmeProcessGetLogPageCompletion(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT status, USHORT commandId);
BOOLEAN NvmeProcessIoCompletion(IN PHW_DEVICE_EXTENSION DevExt);
VOID NvmeRingDoorbell(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT QueueId, IN BOOLEAN IsSubmission, IN USHORT Value);
BOOLEAN NvmeCreateIoCQ(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeCreateIoSQ(IN PHW_DEVICE_EXTENSION DevExt);
int NvmeBuildReadWriteCommand(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PNVME_COMMAND Cmd, IN USHORT CommandId);
USHORT NvmeBuildCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
USHORT NvmeBuildFlushCommandId(IN PSCSI_REQUEST_BLOCK Srb);
PSCSI_REQUEST_BLOCK NvmeGetSrbFromCommandId(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT CommandId);
BOOLEAN NvmeIdentifyController(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeIdentifyNamespace(IN PHW_DEVICE_EXTENSION DevExt);
BOOLEAN NvmeIdentifyEx(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG NamespaceId, IN ULONG CNS, IN USHORT CommandId, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN NvmeGetLogPageEx(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR LogPageId, IN ULONG NamespaceId, IN USHORT CommandId, IN ULONG NumDwords);
VOID NvmeToAtaIdentify(IN PHW_DEVICE_EXTENSION DevExt, OUT PATA_IDENTIFY_DEVICE_STRUCT AtaIdentify);
BOOLEAN NvmeLogPageToScsiLogPage(IN PNVME_SMART_INFO NvmeSmart, IN UCHAR ScsiPageCode, OUT PVOID ScsiLogBuffer, IN ULONG BufferSize, OUT PULONG BytesWritten);

// SCSI helper functions
BOOLEAN ScsiParseSatCommand(IN PSCSI_REQUEST_BLOCK Srb, OUT PUCHAR AtaCommand, OUT PUCHAR AtaFeatures, OUT PUCHAR AtaCylLow, OUT PUCHAR AtaCylHigh);
UCHAR ScsiGetLogPageCodeFromSrb(IN PSCSI_REQUEST_BLOCK Srb);

// helpers for completing SRBs
BOOLEAN ScsiSuccess(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiBusy(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiError(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR SrbStatus);
BOOLEAN ScsiPending(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Sr, IN int Next);

//
// SMART/IOCTL functions
//
BOOLEAN HandleIO_NVMENT(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN HandleIO_SCSIDISK(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN HandleIO_NvmeMini(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);

// SMART IOCTL handler functions
BOOLEAN HandleSmartGetVersion(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PSRB_IO_CONTROL srbControl);
BOOLEAN HandleSmartIdentify(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PSRB_IO_CONTROL srbControl);
BOOLEAN HandleSmartReadAttribs(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PSRB_IO_CONTROL srbControl);
BOOLEAN HandleSmartEnableDisable(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PSRB_IO_CONTROL srbControl, IN BOOLEAN Enable);
BOOLEAN HandleSmartReturnStatus(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb, IN PSRB_IO_CONTROL srbControl);
BOOLEAN ScsiHandleInquiry(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleReadCapacity(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleReadCapacity16(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleReadWrite(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleFlush(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleLogSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleSatPassthrough(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleModeSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleReadDefectData10(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleVerify(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleUnmap(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleRequestSense(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleStartStop(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleModeSelect(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN ScsiHandleReportLuns(IN PHW_DEVICE_EXTENSION DevExt, IN PSCSI_REQUEST_BLOCK Srb);

//
// Completion handlers
//
VOID NvmeProcessUserExtensionCompletion(IN PHW_DEVICE_EXTENSION DevExt, IN USHORT commandId, IN USHORT status, IN PNVME_COMPLETION cqEntry);

//
// PRP list page allocator
//
UCHAR AllocatePrpListPage(IN PHW_DEVICE_EXTENSION DevExt);
VOID FreePrpListPage(IN PHW_DEVICE_EXTENSION DevExt, IN UCHAR PageIndex);
PVOID GetPrpListPageVirtual(IN PHW_DEVICE_EXTENSION DevExt, IN UCHAR PageIndex);
PHYSICAL_ADDRESS GetPrpListPagePhysical(IN PHW_DEVICE_EXTENSION DevExt, IN UCHAR PageIndex);

//
// Uncached memory allocator
//
BOOLEAN AllocateUncachedMemory(IN PHW_DEVICE_EXTENSION DevExt, IN ULONG Size, IN ULONG Alignment,
                               OUT PVOID* VirtualAddress, OUT PHYSICAL_ADDRESS* PhysicalAddress);

#endif // _NVMENT_H_
