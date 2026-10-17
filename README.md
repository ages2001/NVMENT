# NVMENT — NVMe Driver for Windows 95/98/Me and Windows NT 3.1 to XP x64

**NVMENT** is a SCSI miniport driver for NVMe SSDs. It lets Windows 95, 98 and Me, Windows NT 3.1, 3.50, 3.51 and 4.0, and Windows 2000, XP and Server 2003 (x86 and x64, also Alpha AXP) install on, boot from and use NVMe drives — a storage interface that did not exist when these systems were written.

> **NVMENT is based on [nvme2k](https://github.com/techomancer/nvme2k) by DominBear (Dominik Behr).** It continues nvme2k 1.5.0.0 under a new name, with the source layout of its sibling drivers AHCINT, IDENT and MMCSDNT, a Windows 9x build and the additions listed in the [version history](#version-history). The original copyright and the 3-Clause BSD license are kept.

| | |
|---|---|
| **Version** | 3.0.0.0 |
| **Operating systems** | Windows 95 / 98 / Me · Windows NT 3.1 / 3.50 / 3.51 / 4.0 · Windows 2000 / XP / Server 2003 (x86, Alpha) · Windows XP x64 / Server 2003 x64 |
| **Hardware** | NVMe controllers (PCI class code `01-08-02`), NVMe 1.0 and later, up to 8 per system |
| **Devices** | Up to 8 namespaces per controller, each a disk (512, 1024, 2048 and 4096-byte LBA formats) |
| **Binaries** | `nvment9x.mpd` (Windows 9x/Me) · `nvment.sys` (NT 3.1–4.0) · `nvment.sys` (2000/XP/2003, x86 and x64) |
| **License** | 3-Clause BSD — see [License](#license) |

## Highlights

- **Install and boot** on every target: F6 / OEM driver disks for text-mode setup from NT 3.1 to XP x64, Have Disk for installed systems, Device Manager on Windows 9x
- **Queued I/O** with tagged command queuing on the NT family, completed by interrupt, with a watchdog that falls back to Fast Polling where interrupts do not arrive
- **TRIM** through Dataset Management (SCSI UNMAP), plus `tools\trimnt.exe` to deallocate the free space of a volume on systems that never send TRIM
- **Health**: SMART through the `atapi.sys`-compatible "SCSIDISK" IOCTL, ATA PASS-THROUGH IDENTIFY/SMART, and LOG SENSE pages for temperature, wear and critical warnings
- **Power management**: shutdown notification at system shutdown, hibernation and crash dumps on 2000/XP, power states for START STOP UNIT, autonomous power state transitions on request
- **Robust**: controller reset and recovery after a hang, namespaces with large LBA formats, controllers without namespaces handled cleanly
- **One source tree** for all targets, with the same SCSI translation layer and IOCTL conventions as AHCINT, IDENT and MMCSDNT

## Quick start

| System | Files | How |
|---|---|---|
| Windows 95 / 98 / Me | `bin\9X` | [Installing on Windows 95 / 98 / Me](#windows-95--98--me) |
| Windows NT 3.1 – 4.0 | `bin\NT\floppy` | [Installing on Windows NT](#windows-nt-31--350--351--nt-40) |
| Windows 2000 / XP / 2003, installed | `bin\2KXP` (Have Disk) | [GUI-mode setup](#gui-mode-setup-windows-2000xpxp-x64server-2003) |
| Windows 2000 / XP / 2003, new installation | `bin\2KXP\floppy_i386` or `floppy_amd64` (F6) | [Text-mode (F6) setup](#text-mode-f6-setup--windows-2000xpxp-x64server-2003) |

## Contents

- [Overview](#overview) · [Features](#features) · [Architecture](#architecture) · [Feature matrix by target](#feature-matrix-by-target) · [Known Limitations](#known-limitations)
- [Building from source](#building-from-source) · [Installation](#installation) · [Configuration](#configuration) · [Debugging](#debugging) · [Technical Notes](#technical-notes)
- [Version history](#version-history) · [Related drivers](#related-drivers) · [License](#license)

## Overview

NVMENT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It drives the NVMe controller directly — an admin queue and an I/O queue pair, PRP lists for scatter/gather — and translates the SCSI requests of the Windows storage stack into NVMe commands, so an NVMe SSD looks like an ordinary SCSI disk to the class drivers and file systems of these systems.

### Why?

It seemed like a good idea at first.

## Features

- **NVMe**
  - NVMe 1.0 and later; one admin queue and one I/O queue pair, PRP lists from a pool of 32 pages
  - Up to 2 MB per command (limited by the controller's MDTS)
  - **Namespaces**: up to 8 active namespaces, each a LUN of target 0 (Identify CNS 02h on NVMe 1.1+ controllers, otherwise NSID 1..NN), with 512, 1024, 2048 and 4096-byte LBA formats
  - Volatile write cache control (Set/Get Features 06h), FUA, flush
  - Dataset Management (deallocate) for UNMAP and TRIM mode, NVMe Verify for VERIFY
  - Power states: power state 0 while running; START STOP UNIT and the POWER_DOWN IOCTL move the controller to its deepest non-operational state or shut it down; autonomous power state transitions (APST) with `APST=1`
  - Controller reset and recovery: a hung controller (CSTS.CFS, command time-outs) is reset and set up again, and outstanding requests are returned for ScsiPort to retry
  - A controller without namespaces (or whose namespaces are all unusable) presents no LUN; a namespace with no capacity answers NOT READY

- **SCSI translation**
  - INQUIRY with VPD pages `00h`, `80h` (serial number), `B0h` (block and unmap limits), `B1h` (non-rotating, form factor) and `B2h` (logical block provisioning)
  - READ/WRITE(6/10/12/16), VERIFY(6/10/12/16), SYNCHRONIZE CACHE(10/16), READ CAPACITY(10/16) with LBPME, UNMAP, START STOP UNIT, REPORT LUNS, REQUEST SENSE, MODE SENSE/SELECT(6/10) caching page (WCE, DPOFUA)
  - LOG SENSE pages `00h`, `0Dh` (temperature), `11h` (solid state media: percentage used) and `2Fh` (informational exceptions: ASC 5Dh on a critical warning)
  - SMART through the "SCSIDISK" `IOCTL_SCSI_MINIPORT` (as `atapi.sys`), ATA PASS-THROUGH(12/16) for IDENTIFY and SMART
  - NVMe status codes translated to sense data (NVM Express SCSI Translation Reference); sense data kept for REQUEST SENSE when ScsiPort turns auto-sense off
  - Tagged command queuing, ordered tags with an automatic flush
  - **TRIM mode** for Windows versions that send no TRIM: with `tools\trimnt.exe` the free space of a volume is deallocated; see [TRIM mode and trimnt](#trim-mode-and-trimnt)

- **Interrupt or Fast Polling**
  - Interrupt (INTx) mode where the HAL routes PCI interrupts (NT 3.50 to XP x64)
  - **Fast Polling**: completions polled in `HwStartIo`, no interrupt needed — the default on NT 3.1 and Windows 9x, with `IRQ=0`, and in the crash dump / hibernation stack
  - A 100 ms watchdog completes what a lost interrupt left behind and switches to Fast Polling for good after five such cases

- **Power management**
  - System shutdown: flush and shutdown notification (CC.SHN), so the SSD saves its tables before the power goes
  - Windows 2000/XP: `ScsiStopAdapter` (shutdown notification, controller disabled) and `ScsiRestartAdapter` (set up again) for standby and hibernation; the dump stack instance writes the hibernation file with Fast Polling
  - NT 3.1–4.0 and 9x: `HwAdapterState` and the POWER_DOWN / POWER_UP IOCTLs for an ACPI or APM driver

- **Multi-target support from one source tree**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`nvment9x.mpd`)
  - Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86) — `nvment.sys` (one binary for all four)
  - Windows 2000 / XP / Server 2003 (x86, Alpha) and Windows XP x64 / Server 2003 x64 (amd64) — `nvment.sys`

## Architecture

```
          Windows 9x/Me                         Windows NT/2000/XP

        Application Layer                       Application Layer
               ↓                                       ↓
     IOS (I/O Supervisor) layers                SCSI Class Driver
               ↓                                       ↓
          SCSIPORT.PDR                            ScsiPort.sys
               ↓                                       ↓
          nvment9x.mpd                            nvment.sys
      (SCSI miniport driver)                 (SCSI miniport driver)
               ↓                                       ↓
               └──── NVMe controller (PCI, 01-08-02) ──┘
                                    ↓
                              NVMe namespaces
```

### Key Components

| File | Purpose |
|---|---|
| `src\nvmemain.c` | `DriverEntry`, `HwFindAdapter` (PnP resources, own PCI scan on NT 3.1, 9x claim table), `HwInitialize`, `HwStartIo`, `HwInterrupt`, the watchdog, `HwResetBus`, `HwAdapterControl` / `HwAdapterState`, the 9x VMM mapping |
| `src\nvmecmd.c` | Controller bring-up and shutdown, admin commands (Identify, features, log pages, APST), I/O submission, PRP lists, power states, recovery |
| `src\nvmecpl.c` | Completion queue processing |
| `src\nvmesatl.c` | SCSI translation, sense data, MODE/LOG SENSE, UNMAP, VERIFY, SMART and the IOCTLs |
| `src\nvmeutil.c`, `src\nvmeutil.h` | Helpers: PCI access, argument parsing, SMART/log page translation, signatures |
| `src\nvment.h`, `src\nvme.h`, `src\scsiext.h` | Driver structures and limits, NVMe definitions, SCSI additions |
| `src\nvment.rc` | Version resource (`nvment.sys` / `nvment9x.mpd`) |
| `src\2KXP`, `src\NT`, `src\9X` | Build files per target |
| `bin\` | Prebuilt binaries and install files |
| `tools\trimnt.c` | TRIM of a volume's free space (all four sibling drivers) |

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.1/3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86, Alpha) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Build folder (sources are shared in `src\`) | `src\9X` | `src\NT` | `src\2KXP` |
| Target define | `NVMENT_W9X`, `_WIN32_WINNT=0x400` | `_WIN32_WINNT=0x400` (NT 3.1 support built in) | `_WIN32_WINNT=0x500` |
| Driver binary | `nvment9x.mpd` | `nvment.sys` | `nvment.sys` |
| I/O model | Fast Polling; interrupt with `IRQ=1` | Interrupt on NT 3.50–4.0; NT 3.1 Fast Polling unless `IRQ=1`; Fast Polling with `IRQ=0` | Interrupt on the PnP-assigned line; Fast Polling in the dump stack and with `IRQ=0` |
| Requests in flight per namespace | One | Many (tagged queuing) | Many (tagged queuing) |
| Controller discovery | Bus:slot from `SCSIPORT.PDR`, then a bus scan (claim table for several controllers) | ScsiPort (NT 3.50+), own scan through ports `CF8h`/`CFCh` on NT 3.1 | PnP resources |
| Register mapping | VMM `_MapPhysToLinear` | `ScsiPortGetDeviceBase` | `ScsiPortGetDeviceBase` |
| DMA memory | Page by page (each page its own physical address) | One block; EISA bus on NT 3.1 so it is not limited to 16 MB | One block |
| Memory above 4 GB | Not used by 9x | Not used by NT 3.1–4.0 | 64-bit PRP addresses |
| Power management | `HwAdapterState`; power IOCTL | Shutdown notification; power IOCTL | `HwAdapterControl` stop/restart, hibernation, crash dump; power IOCTL |
| UNMAP, TRIM mode, LOG SENSE, SMART | Yes | Yes | Yes |
| Debug output (`NVMENT_DBG` builds) | `ScsiDebugPrint` | `ScsiDebugPrint` | `ScsiDebugPrint` |

## Known Limitations

- Single I/O queue pair (no multi-queue)
- No MSI/MSI-X; the interrupt path uses legacy INTx (a controller that only does MSI runs with Fast Polling once the watchdog notices)
- At most 32 (or 16 in the fallback) large transfers in flight at once, limited by the PRP list page pool
- No namespace management (namespaces are only discovered); namespaces with metadata (MS ≠ 0) or protection information are skipped
- No Host Memory Buffer: DRAM-less SSDs that rely on HMB run at their slower no-HMB speed (ScsiPort gives a miniport only one block of DMA memory, before the controller can be asked how much HMB it wants)
- Windows 9x: Fast Polling unless `IRQ=1`; `SCSIPORT.PDR` sends one request per device at a time
- Windows 9x and BIOS-served disks: an NVMe disk the BIOS also serves through INT 13h (an NVMe option ROM, SeaBIOS) can appear twice, once through nvment9x and once in MS-DOS compatibility mode (a drive that hangs when accessed), as with AHCINT and MMCSDNT; do not use the compatibility-mode drive
- Windows before 7 sends no TRIM: deallocation happens through UNMAP pass-through or `trimnt` only
- APST is off by default and was not tested on hardware (QEMU's controller has none)
- Tested primarily in virtual machines (QEMU); nvme2k was also tested on Windows 2000 RC2 on Alpha. Reports from real hardware are welcome

## Building from source

### Requirements

| Target | Tools |
|---|---|
| Windows 95 / 98 / Me | Windows 95 or 98 DDK (`MASTER.MK`, `BLOCK\LIB\scsiport.lib`), Visual C++ 2.0 or later 32-bit compiler, MASM 6.11, a Windows 9x build machine (`COMMAND.COM` batch) |
| Windows NT 3.1 / 3.50 / 3.51 / 4.0 | Windows NT 4.0 DDK, Visual C++ 4.x |
| Windows 2000 / XP / Server 2003 (x86, Alpha) | Windows 2000 DDK (5.00.2195.1; RC1 for Alpha), Visual C++ 6.0 |
| Windows XP x64 / Server 2003 x64 | Windows Server 2003 SP1 DDK (3790.1830) or WDK 6001.18002 / 7600.16385.1 |
| Any (cross build) | MinGW-w64 (i686 / x86_64) with import libraries of the target's `scsiport.sys` / `ntoskrnl.exe` |

### Build Instructions

#### For Windows 95 / 98 / Me

Copy the repository's `src` folder to the build machine, edit the paths at the top of `src\9X\build.bat` (`MASM_ROOT`, `C32_ROOT`, `DDKROOT`, and `NVMENT_ROOT` = the copied `src` folder) and the library path in `nvment9x.lnk`, and run `build.bat` from a `COMMAND.COM` prompt. `set DEBUG=1` first gives a debug build. Output: `9X\nvment9x.mpd`; `nmake clean` removes the build output.

#### For Windows NT 3.1 / 3.50 / 3.51 / NT 4.0

```cmd
cd C:\DDK\bin
setenv.bat C:\DDK free
cd <path-to-NVMENT>\src\NT
build -cZ
```

The NT build runs on NT 3.1, 3.50, 3.51 and 4.0: it imports only ScsiPort functions NT 3.1 has and adapts at run time to NT 3.1's smaller `HW_INITIALIZATION_DATA` and `PORT_CONFIGURATION_INFORMATION`.

#### For Windows 2000 / XP / Server 2003 (x86, Alpha) and x64

```cmd
cd C:\NTDDK\bin
setenv.bat C:\NTDDK free          REM or 'checked' for a debug build
cd <path-to-NVMENT>\src\2KXP
build -cZ
```

Output: `obj\i386\nvment.sys` (`obj\alpha\`, or the amd64 output directory of an x64 build environment). The x64 driver is built from the same folder in an amd64 ("WNET") build environment.

#### With MinGW-w64 (cross build)

Compile every `.c` file in `src\` with `-ffreestanding -fno-builtin`, `-mrtd` on x86 (stdcall, like the DDK's `/Gz`), `_WIN32_WINNT=0x400` for the NT 3.1–4.0 build or `0x500` for 2000/XP/x64, and link against import libraries of `scsiport.sys` (and `ntoskrnl.exe` on x64). For Windows 9x add `-DNVMENT_W9X -fpack-struct=4`, use `HwInitializationDataSize` 4Ch and export `DriverEntry`. In the `-mrtd` build `__udivmoddi4` and friends must come from a stdcall-aware helper, not from libgcc. `nvment.rc` compiles with `windres` (`-DNVMENT_W9X` for `nvment9x.mpd`).

### Build Options

```c
#define NVMENT_DBG                    // debug output (ScsiDebugPrint)
// #define NVMENT_DBG_CMD             // extra verbose command logging
// #define NVMENT_DBG_EXTRA           // now that is just excessive
```

## Installation

### Installation Media

Prebuilt binaries and install files are in `bin\`:

```
bin\
├── 9X\                    (Windows 95 / 98 / Me)
│   ├── nvment9x.inf
│   └── nvment9x.mpd
├── NT\                    (NT 3.1 / 3.50 / 3.51 / 4.0)
│   ├── nvment.sys
│   ├── oemsetup.inf
│   └── floppy\            (driver disk)
│       ├── disk1          (tag file)
│       ├── oemsetup.inf
│       ├── txtsetup.oem
│       └── nvment.sys
└── 2KXP\                  (Windows 2000 / XP / Server 2003, x86 and x64)
    ├── nvment.inf         (one INF for x86, x64 and Alpha; Have Disk)
    ├── nvment.sys         (x86, for a flat copy)
    ├── i386\nvment.sys
    ├── amd64\nvment.sys
    ├── floppy_i386\       (F6 disk, 2000/XP/2003 x86: disk1, txtsetup.oem, flat nvment.inf, nvment.sys)
    └── floppy_amd64\      (F6 disk, XP x64/2003 x64: the same for amd64)
tools\
├── trimnt.exe
└── trimnt.c
```

The F6 disks carry a flat copy of `nvment.inf` (no `i386\`/`amd64\` subdirectories): text-mode setup copies the driver and the INF flat into `%SystemRoot%\OemDir`, and GUI-mode setup installs the controller again from there — with the subdirectories it would ask for the file ("Files Needed").

Every INF matches the NVMe class code `PCI\CC_010802`, so any NVMe controller is offered.

### Installing

#### Windows 95 / 98 / Me

Windows 9x/Me setup itself runs through the BIOS (INT 13h), so there is no F6-style driver step — install nvment9x once Windows is up. Copy `bin\9X\nvment9x.inf` and `nvment9x.mpd` to a folder on the hard disk (for example `C:\NVMENT`), then:

1. Let the **New Hardware Found** wizard pick up the controller, or open **Device Manager**, where it is listed as an unknown **PCI Mass Storage Controller** (or under **Other devices**).
2. Choose **Driver from disk** (wizard) or **Properties → Driver → Update Driver** (Windows 98/Me; Windows 95: **Change Driver → Have Disk**) and point it at the folder.
3. Select **nvment9x NVMe Storage Controller** and reboot. `nvment9x.mpd` is copied to `WINDOWS\SYSTEM\IOSUBSYS`.

If the protected-mode driver fails to load, Windows 9x falls back to MS-DOS compatibility mode for the affected disks (check **Device Manager → Performance** and `BOOTLOG.TXT`).

> **Windows 95 RTM users:** if the controller shows a **yellow exclamation mark** in Device Manager, the original Windows 95 `SCSIPORT.PDR` is the problem. Either install the [AMDK6 Update](https://winworldpc.com/download/c3a0c2b0-c398-c2a8-3e02-11c3a6e28094), or extract `SCSIPORT.PDR` from that update and copy it to `WINDOWS\SYSTEM\IOSUBSYS` yourself — rename the existing `SCSIPORT.PDR` there to `SCSIPORT.BKP` first so you keep a backup.

#### GUI-mode setup (Windows 2000/XP/XP x64/Server 2003)

In Device Manager (or the Found New Hardware / Add Hardware wizard) choose **Have Disk**, point it at `bin\2KXP` and select **NVMENT NVMe Storage Controller**. The same INF serves x86 (`i386\nvment.sys`), x64 (`amd64\nvment.sys`) and Alpha. The driver installs as service `NVMENT`, group `SCSI Miniport`, boot start.

#### Text-mode (F6) setup — Windows 2000/XP/XP x64/Server 2003

Copy the contents of `bin\2KXP\floppy_i386\` (x86) or `bin\2KXP\floppy_amd64\` (x64) onto a floppy disk (or a virtual floppy image), press **F6** at the start of text-mode setup and select the NVMENT controller. Setup then lists the namespaces as disks, partitions, formats and installs onto them; GUI-mode setup installs the controller again from the flat INF in `OemDir`.

Text-mode setup starts the driver with a bare service key — the `[Config]` values of `txtsetup.oem` only reach the installed system — and the XP x64 / Server 2003 x64 ScsiPort fails to start the controller without `Parameters\PnpInterface\5`. `DriverEntry` therefore creates that value itself when the `PnpInterface` key is missing.

#### Windows NT 3.1 / 3.50 / 3.51 / NT 4.0

- **Installed system**: NT 3.x: *Windows NT Setup* → *Options* → *Add/Remove SCSI Adapters* → *Add* → *Other (requires disk)*. NT 4.0: *Control Panel* → *SCSI Adapters* → *Drivers* → *Add* → *Have Disk*. Give `bin\NT\floppy`.
- **Text-mode setup** (system disk on NVMe): press **S** at the mass storage screen (F6 on NT 4.0) and give the driver disk `bin\NT\floppy`.

The miniport is installed as a boot driver (`Start = 0`). `oemsetup.inf` also sets the disk class driver (`scsidisk` on NT 3.x, `disk` on NT 4.0) to start at boot: a class driver that starts at boot while the miniport starts later does not claim the disks, and the NVMe disks would not show up at all.

NT 3.1 resolves the boot.ini `multi()` path by disk order. If the system disk is on another controller (IDE, AHCI), that controller's miniport has to load before NVMENT, otherwise the NVMe disks come first and the boot stops with STOP 0x6B. `oemsetup.inf` therefore sets no `Tag`; `txtsetup.oem`, used for a system disk on NVMe, sets `Tag = 33`.

## Configuration

Registry values set at install time on the NT family (`nvment.inf`, `txtsetup.oem`, `oemsetup.inf`):

- **Type / Start / ErrorControl** – `1` (kernel driver) / `0` (boot start) / `1` (normal); **Group** – `SCSI Miniport`
- **Tag** – `33` when installed through `txtsetup.oem` (system disk on NVMe); none through `oemsetup.inf` (see above)
- **`Parameters\Device`** – `MaximumSGList` (255; 512 on NT 3.1–4.0) and `NumberOfRequests` (32)
- **2000/XP/x64 only** – `Parameters\PnpInterface\5 = 1` (PnP on the PCI bus); created by the driver when missing

On Windows 9x/Me, `nvment9x.inf` registers the controller with `DevLoader = *IOS` and `PortDriver = nvment9x.mpd` (plus `DontLoadIfConflict = Y`).

### Driver parameters

On the NT family the driver reads the ScsiPort argument string: the `REG_SZ` value `DriverParameters` (or `DriverParameter`) under `HKLM\SYSTEM\CurrentControlSet\Services\NVMENT\Parameters\Device` (create the keys if they are missing; reboot to apply). On Windows 95/98/Me `SCSIPORT.PDR` passes the string value `AdapterSettings` of the controller's driver key (`HKLM\System\CurrentControlSet\Services\Class\SCSIAdapter\000n`, the one whose `PortDriver` is `nvment9x.mpd`), for example `"AdapterSettings"="IRQ=1"`.

| Value | Effect |
|---|---|
| `IRQ=0` | No interrupt: every command is Fast Polling (any version) |
| `IRQ=1` | Windows 9x, and NT 3.1 (no PCI slot information): connect the interrupt line from PCI configuration space |
| `APST=1` | Autonomous power state transitions on (controllers that report APSTA) |

Several values are separated by spaces. The crash dump / hibernation instance is recognized by the `DUMP=` string ScsiPort passes to it.

### Build-time limits

```c
#define NVMENT_MAX_LUNS                 8       // namespaces per controller
#define NVMENT_MAX_SG_PAGES             32      // PRP list page pool
#define NVMENT_MAX_CONTROLLERS          8
#define NVMENT_WATCHDOG_USEC            100000  // interrupt watchdog while commands are outstanding
#define NVMENT_MISSED_IRQ_LIMIT         5       // lost interrupts before Fast Polling
#define NVMENT_APST_MAX_LATENCY_US      100000  // APST: deepest state with an exit latency up to 100 ms
```

## Debugging

A debug build (`NVMENT_DBG`) writes its messages through `ScsiDebugPrint()`, prefixed `NVMENT:` — to the kernel debugger (WinDbg, i386kd) on the NT family and to the debug terminal on Windows 9x (`set DEBUG=1` builds). Useful kernel debugger commands:

```
!scsiport.miniport <DeviceExtension>
!devobj <DeviceObject>
```

## Technical Notes

### I/O Model

- **Interrupt:** a read or write is submitted to the I/O submission queue and `HwStartIo` returns; `HwInterrupt` processes the completion queues and completes the requests. With tagged queuing ScsiPort keeps sending requests while earlier ones are in flight. While commands are outstanding a 100 ms ScsiPort timer also processes the queues; when it finds completions that no interrupt reported five times, interrupts are masked (INTMS) and the driver continues with Fast Polling.
- **Fast Polling:** the command is submitted and the queues are polled every 10 µs until it completes, at most until the request's own timeout minus one second (10 s when the request carries none, 60 s at most), so ScsiPort's own timeout handling never races a request the miniport is about to complete.

### IOCTL interface

`IOCTL_SCSI_MINIPORT` requests to `\\.\ScsiN:` with the signature `NVMENT  ` (the old `NVME2KDB` is still accepted):

| Code | Function |
|---|---|
| `1000h` QUERY_INFO | Optional `NVMENT_INFO` after the header: driver and NVMe version, flags (bit 0 Fast Polling, bit 1 powered down, bit 2 write cache, bit 3 TRIM mode, bit 4 APST), LUN count, power state, maximum transfer, deepest queue, interrupt count, NSID / block size / size of every LUN |
| `1001h` TRIM_MODE_ON | A 4 KB pattern after the header; writes of that pattern become deallocates |
| `1002h` TRIM_MODE_OFF | Back to normal writes |
| `1010h` POWER_DOWN | Waits for queued commands, shutdown notification, controller disabled; the next request or POWER_UP brings it back |
| `1011h` POWER_UP | Set the controller up again |

SMART tools reach the controller through the "SCSIDISK" signature (`IOCTL_SCSI_MINIPORT_SMART_VERSION`, `_IDENTIFY`, `_READ_SMART_ATTRIBS`, `_RETURN_STATUS`), answered from Identify Controller and the SMART / Health log page, as `atapi.sys` answers them for ATA disks.

### TRIM mode and trimnt

Windows versions before 7 never send TRIM or UNMAP for deleted files. `TRIM_MODE_ON` gives the driver a 4 KB pattern; while the mode is on, a write whose data is that pattern repeated over the whole request is deallocated (Dataset Management) instead of written. `tools\trimnt.exe <drive letter>` switches the mode on with a random pattern on every AHCINT, IDENT, MMCSDNT and NVMENT adapter, fills the free space of the volume with files of that pattern (unbuffered and write-through, so every write reaches the driver), switches the mode off and deletes the files. Ctrl+C stops it early, and the mode is switched off and the files deleted as usual. Other writes are written normally all the time.

### LOG SENSE

| Page | Contents |
|---|---|
| `00h` | Supported pages |
| `0Dh` | Temperature (composite temperature of the SMART / Health log) |
| `11h` | Solid state media: percentage used endurance indicator |
| `2Fh` | Informational exceptions: ASC 5Dh when the SMART / Health log reports a critical warning, the temperature, then the vendor parameters of nvme2k |

### APST

With `APST=1` and a controller that reports APSTA (Identify Controller byte 265), the driver builds the autonomous power state transition table (Set Features 0Ch) from the power state descriptors: every power state goes to the deepest non-operational state below it whose exit latency is at most 100 ms, after an idle time of 50 times that state's entry plus exit latency (the rule Linux uses). It is off by default because some early controllers and virtual machines handle APST badly.

### Windows 9x specifics

- **Register mapping:** the BAR is mapped with the VMM's `_MapPhysToLinear` service, as in AHCINT and MMCSDNT (Windows 95's `ScsiPortGetDeviceBase` can fail for memory BARs).
- **DMA memory:** `SCSIPORT.PDR`'s uncached extension is not physically contiguous; the queues and every PRP list page get their own physical address.
- **Several controllers:** `SCSIPORT.PDR` calls `HwFindAdapter` for every PCI function; a claim table keeps one controller from being taken twice.
- **`HwAdapterState`:** provided, as `SCSIPORT.PDR` requires it for PCI miniports.
- **One request per device** at a time and Fast Polling by default (`IRQ=1` connects the interrupt).

### Windows NT 3.1 specifics

The NT build handles NT 3.1 at run time, so the same `nvment.sys` runs on NT 3.1, 3.50, 3.51 and 4.0:

- **`HW_INITIALIZATION_DATA`:** NT 3.1's `ScsiPortInitialize` takes only its own 40h-byte structure; the driver retries with that layout on the ISA bus when the NT 3.50–4.0 call fails.
- **PCI:** NT 3.1's HAL has no PCI configuration access; the driver scans the bus itself through configuration mechanism #1 (ports `CF8h`/`CFCh`).
- **DMA memory:** taken as EISA on buses without PCI slot information, so it is not limited to the first 16 MB.
- **Interrupt:** Fast Polling by default, since a failed interrupt connection fails the adapter there; `IRQ=1` connects the INTx line.

### Windows 2000/XP/x64 specifics

- **PnP adapter:** the PCI bus driver enumerates the controller and ScsiPort starts NVMENT with its BAR and interrupt.
- **Text-mode setup (F6):** `Parameters\PnpInterface\5` is created by `DriverEntry` when missing (see [Text-mode (F6) setup](#text-mode-f6-setup--windows-2000xpxp-x64server-2003)).
- **Hibernation and crash dump:** the dump stack instance runs with Fast Polling and a small (16 KB) DMA area; its queues are aligned on the physical address, since the dump stack's DMA buffer does not start on a page boundary.

### Memory Allocation

- **Uncached extension** – about 152 KB for the queues, PRP lists and the TRIM pattern (16 KB in the dump stack)
- **Admin queue** – 4 KB submission + 4 KB completion
- **I/O queue** – 4 KB submission + 4 KB completion
- **PRP list pool** – 32 (or 16) pages for scatter/gather

### Command ID Encoding

```
I/O queue:
Bit 15: non-tagged flag (1 = non-tagged, 0 = tagged)
Bit 14: ordered flush flag (for ORDERED queue tags)
Bits 8-10: LUN (namespace index)
Bits 0-7: QueueTag (tagged) or sequence number (non-tagged)

Admin queue:
Bits 11-13: kind (1 synchronous/polled, 2 GET LOG PAGE for SMART,
            3/4 user IDENTIFY / GET LOG PAGE)
Bits 0-7: PRP slot or sequence number
For a GET LOG PAGE admin command the PRP slot is added to the base CID,
so the slot is not leaked when ScsiPort does not return the SRB (it does
not for the SMART requests).
```

## Version history

### 3.0.0.0 — 2026-10-12 (NVMENT)

- Renamed nvme2k to NVMENT: `nvment.sys`, service `NVMENT`, `nvment.inf` / `oemsetup.inf` / `nvment9x.inf`; IOCTL signature `NVMENT  ` (the old `NVME2KDB` is still accepted).
- New source layout shared with AHCINT, IDENT and MMCSDNT: sources in `src\`, build files in `src\2KXP`, `src\NT` and `src\9X`, prebuilt files in `bin\`.
- Added Windows 95, 98 and Me support (`nvment9x.mpd`).
- Added LOG SENSE pages for temperature (0Dh), wear (11h) and critical warnings (2Fh, ASC 5Dh).
- Added autonomous power state transitions (`APST=1`).
- Fixed the shutdown notification at system shutdown: ScsiPort now passes `SRB_FUNCTION_SHUTDOWN` to the driver.
- Fixed a failed pass-through command returning an I/O error instead of its sense data (sense data is kept for REQUEST SENSE when auto-sense is off).
- Added TRIM mode flag and APST flag to QUERY_INFO; the `trimnt` tool replaces the old `trim` tool.
- Added version information to the driver files (file properties show 3.0.0.0).

### nvme2k 1.5.0.0 — 2026-10-07

- Windows NT 3.1, 3.5 and 3.51 support (same binary as NT 4.0) with an own PCI scan; DMA memory taken as EISA on buses without PCI slot information.
- Fast Polling (`IRQ=0`, NT 3.1 default, crash dump / hibernation), a watchdog for lost interrupts, controller reset and recovery.
- Namespaces 1–8 as LUNs with 1024, 2048 and 4096-byte LBA formats.
- VERIFY, UNMAP, START STOP UNIT, MODE SELECT (write cache), REPORT LUNS, range checks, FUA, sense data from NVMe status, VPD B0h/B2h.
- ScsiStopAdapter / ScsiRestartAdapter, hibernation through the dump stack, power IOCTLs.
- `oemsetup.inf` also makes the disk class driver a boot driver; F6 disks for x86 and x64.
- A controller without namespaces presents no LUN (Windows 2000 hung at "Starting up" before).

Earlier versions: see the [nvme2k](https://github.com/techomancer/nvme2k) project.

## Related drivers

NVMENT, AHCINT, IDENT and MMCSDNT are one family: the same ScsiPort miniport design, SCSI translation layer, IOCTL conventions, Fast Polling fallback and TRIM mode, each built from one source tree for every system from Windows 95 and NT 3.1 to XP x64.

| Driver | Storage |
|---|---|
| **AHCINT** | SATA disks and optical drives on AHCI controllers |
| **IDENT** | PATA disks and optical drives, and SATA controllers in IDE mode |
| **MMCSDNT** | SD/SDHC/SDXC cards and MMC/eMMC on SD host controllers (SDHCI) |
| **NVMENT** | NVMe SSDs |

## License

This project is licensed under the **3-Clause BSD License**; see the [LICENSE](LICENSE) file. Copyright (c) 2025 Dominik Behr (nvme2k), 2026 ages2001 (NVMENT).

**USE AT YOUR OWN RISK.** This driver is experimental and may cause data loss, system instability, or hardware damage. Not recommended for production use.

## Acknowledgments

- **DominBear (Dominik Behr)** for [nvme2k](https://github.com/techomancer/nvme2k), which NVMENT is based on
- Ray Hinchliffe for build contributions and extensive testing of nvme2k
- SweetLow for taking it even further into the realm of insanity aka win9x ([nvme9x](https://github.com/LordOfMice/Tools))
- infuscomus for porting the sibling drivers to x64
- Dietmar for the Fast Polling idea
- Folks from the Win2kdev Discord and Vogons
- The NVM Express specification authors
- Windows 2000 DDK documentation and the Alpha AXP architecture documentation
- Everyone who thought this was a terrible idea (you were right)

---

**Disclaimer:** This is an unofficial, community-developed driver. Not affiliated with any company or with the NVM Express standards body.
