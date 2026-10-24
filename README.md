# iOS on Google Pixel Fold (Tensor G2 / felix)

[![Milestone](https://img.shields.io/badge/Milestone-11%20Clean%20Boot%20%26%20Apple%20Logo-brightgreen.svg)]()
[![Target OS](https://img.shields.io/badge/Target%20OS-Apple%20iOS%2016.7.16%20(XNU%2022.6.0)-blue.svg)]()
[![Hardware](https://img.shields.io/badge/SoC-Google%20Tensor%20G2%20(GS201)-orange.svg)]()
[![Display](https://img.shields.io/badge/Display-Outer%20AMOLED%201080x2092-red.svg)]()

Running genuine Apple **Darwin XNU 22.6.0** (iOS 16.7.16, Build `20H392`, `D22AP`) bare-metal on the **Google Pixel Fold** (`felix`, Tensor G2 / GS201).

The system boots directly via an AOSP Header v4 boot image (`fastboot boot`), initializes the Exynos UFS 3.1 storage controller, mounts the physical `userdata` partition formatted as HFS+, outputs genuine Apple boot graphics on the physical outer AMOLED display, and bootstraps the full Apple iOS userland (`launchd` &rarr; `securityd` &rarr; `backboardd` &rarr; `SpringBoard`).

---

## Current Status & Achievements

- [x] **Bare-Metal Bootloader:** Custom freestanding AOSP Header v4 bootloader with strictly 0 ABS64 and 0 GOT relocations, signed with AVB test key.
- [x] **Apple DeviceTree Synthesis:** Dynamically synthesizes an Apple DeviceTree (`arm-io`, `interrupt-controller`, `exynos-uart`, `ufsroot0`, `sweet,display`, `chosen`, `memory`).
- [x] **Display & Framebuffer:** Outer AMOLED panel (1080&times;2092 @ 60Hz, 32bpp BGRA) running at physical address `0xFC000000`. Hardware DMA continuous scanout enabled on Exynos DECON (`0x1C133060 = 0x3061`).
- [x] **Physical UFS 3.1 Storage:** Hardware PHY initialization and SCSI command engine communicating with physical NAND; mounts physical GPT `userdata` partition as HFS+ rootfs (`/dev/ufsroot0`).
- [x] **Apple Graphical Boot:** Displays the genuine Apple boot logo and active progress bar directly on the AMOLED panel via `AppleCLCD` / `IOMobileFramebuffer`.
- [x] **iOS Userland Bootstrap:** Apple `launchd` (PID 1) executes system daemons: `securityd`, `tccd`, `lsd`, `keybagd`, `usermanagerd`, etc.
- [x] **SpringBoard & backboardd:** `backboardd` runs multiple SMP event loops; `SpringBoard` attaches to CoreAnimation `CADisplay` for UI rendering.
- [x] **Userland Patches:** Injected `libredwood-sb.dylib` (sets `LOCKDOWN_ACTIVATED = 2`, software vibrancy, and snapshot generation); disabled crash-prone `mobileactivationd` and `watchdogd` loops; injected `purplebuddy` and `migration` configurations.

---

## Hardware Architecture & Memory Map

| Component | Physical Address / Details | Notes |
|---|---|---|
| **SoC** | Google Tensor G2 (`GS201` / `zuma-family`) | 2&times; Cortex-X1, 2&times; Cortex-A78, 4&times; Cortex-A55 |
| **DRAM Window** | Base `0xF0000000` (320 MB total) | Avoids Tensor G2 secure EL3/CARVE-OUT memory regions |
| **Outer AMOLED Framebuffer** | `0xFC000000` (1080 &times; 2092, 32bpp BGRA) | Pitch = 4352 bytes, continuous DECON scanout |
| **Exynos DECON Controller** | `0x1C130000` (Reg `0x3060` = `0x3061`) | Configured for hardware DMA continuous refresh |
| **UFS 3.1 Controller** | `0x14700000` (Vendor slot reg `0x14701140`) | LUN 0 GPT partition `userdata` (~218 GB) |
| **ARM GICv3 Distributor** | `0x10400000` | GICD base |
| **ARM GICv3 CPU / Redist** | `0x10410000` | GICC / GICR (PPI 11 / INTID 27 for Arch Timer) |
| **Exynos UART** | `0x10A00000` | 115200 baud, 8N1 serial console |
| **ARM Generic Timer** | 24.576 MHz | Standard ARM architected timer frequency |

---

## Repository Structure

The repository is organized into three clean, self-contained directories:

```
├── baremetal_loader/           # Standalone AOSP Header v4 baremetal bootloader
│   ├── build.sh                # 1-click build script (Clang -> lld -> LZ4 -> mkbootimg -> avbtool)
│   ├── entry.S                 # Low-level EL1 entry, stack setup, cache invalidation
│   ├── loader_main.c           # Memory setup, ADT synthesis, DECON init, XNU trampoline
│   ├── console.c / console.h   # Early framebuffer text rendering engine & font table
│   ├── ufsinit.c / ufsinit.h   # Exynos UFS 3.1 controller & PHY initialization
│   ├── kernel_sweet_ci_patched.macho # Working patched iOS 16 Darwin XNU kernel Mach-O
│   ├── linker.ld               # Custom ELF linker script ensuring zero relocations
│   ├── mkbootimg.py / avbtool.py # AOSP packaging and AVB v1.2 signing utilities
│   └── test_rsa4096.pem        # AVB signing key
├── tools/                      # Userland automation and patching scripts
│   ├── flash_userland.sh       # All-in-one script to mount, patch, sparse-encode, and flash
│   ├── make-android-sparse.py  # Pure Python converter (raw HFS+ image -> Android sparse .simg)
│   └── userland_patches/       # Required iOS userland injection files
│       ├── lib/                # libredwood-sb.dylib & libredwood.dylib
│       └── preferences/        # com.apple.purplebuddy.plist, migration.plist, springboard.plist...
├── xnu/                        # Reference source code for HackiOS XNU driver extensions
│   └── files/                  # GenericARMPE, MiniAppleKeyStore, apfs, gic_v3_irq, psci_smp...
├── TENSOR_G2_PORTING_MASTER_GUIDE.md # Deep technical reference & MMIO documentation
└── README.md                   # This document
```

---

## Quick Start Guide

### 1. Prerequisites (macOS Host)

Install the required cross-compilation and flashing toolchain via Homebrew:

```bash
brew install llvm lld lz4 android-platform-tools
```

Ensure `clang`, `ld.lld`, and `fastboot` are accessible in your `PATH`.

---

### 2. Building the Baremetal Boot Image

Compile the loader and package the signed Header v4 boot image:

```bash
cd baremetal_loader
./build.sh
```

**Build Output:**
- `baremetal_loader/felix_xnu_boot_v4.img` (64 MB, signed with AVB footer, ready for `fastboot boot`).

---

### 3. Patching & Flashing the iOS Userland

To flash your iOS 16 root filesystem to the physical `userdata` partition on the phone:

```bash
sudo ./tools/flash_userland.sh /path/to/ios-root.img
```

**What this script does automatically:**
1. Attaches `ios-root.img` locally via `hdiutil`.
2. Injects `libredwood-sb.dylib` and `libredwood.dylib` into `/usr/lib/`.
3. Configures `DYLD_INSERT_LIBRARIES` in `com.apple.SpringBoard.plist` and `com.apple.backboardd.plist`.
4. Disables crash loops in `com.apple.mobileactivationd.plist` and `com.apple.watchdogd.plist`.
5. Installs `com.apple.purplebuddy.plist` (`SetupDone = true`) and `com.apple.migration.plist` to `/var/mobile/Library/Preferences/`.
6. Detaches the image cleanly and converts it to Android sparse format (`ios-root-clean.simg`).
7. Flashes the sparse image to the phone: `fastboot flash userdata ios-root-clean.simg`.
8. Prompts to boot the XNU kernel directly.

---

### 4. Booting iOS Bare-Metal

Put the Google Pixel Fold into Fastboot / Bootloader mode (`Volume Down` + `Power`):

```bash
fastboot boot baremetal_loader/felix_xnu_boot_v4.img
```

1. The phone displays the early boot log on the outer AMOLED screen.
2. The XNU kernel initializes MMU, Mach VM, and the UFS driver.
3. The genuine Apple boot logo appears on the screen.
4. Launch Services (`lsd`) indexes LaunchDaemons; the Apple progress bar renders beneath the logo.
5. `SpringBoard` and `backboardd` take over graphics presentation.

---

## Boot Arguments & Configuration

The boot arguments passed from `baremetal_loader/loader_main.c` to XNU in the DeviceTree `/chosen` node:

```
-v debug=0xc serial=3 cpus=1 keepsyms=1 wdt=-1 wdt_test=no_user_checkin sweetusb=0 sweetlog=0 rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 sweetfb_native=1
```

### Key Boot-Args Explained:

- `sweetfb_native=1`: Notifies `AppleCLCD` to register the display as an Apple native display, enabling CoreAnimation `CADisplay` binding.
- `sweetfb_noconsole=1` *(optional)*: Suppresses kernel text rendering on the framebuffer to keep the Apple logo and SpringBoard completely clean.
- `-enable_kprintf_spam` *(optional for debugging)*: Routes all `kprintf()` output to the screen framebuffer. *(Note: will draw text over the UI if left enabled during full SpringBoard operation).*
- `rd=ufsroot0 ufsrw=1 rootrw=1`: Mounts the physical UFS NAND `userdata` partition as a read/write rootfs.
- `wdt=-1 wdt_test=no_user_checkin`: Disables kernel and launchd hardware watchdogs to prevent watchdog reboots during slow single-core cold boots.

---

## Known Quirks & Lifehacks

### 1. Launch Services (`lsd`) Initial Indexing
On a freshly flashed `userdata` partition, `lsd` must index all 532 system LaunchDaemons. Under software rasterization on a single core (`cpus=1`), the Apple progress bar under the boot logo may take **3 to 7 minutes** to complete. Do not interrupt or reboot the device during this stage.

### 2. FrontBoard Watchdogs
To prevent SpringBoard from killing scenes during cold boot delays, watchdog timeouts are overridden in `/private/var/mobile/Library/Preferences/com.apple.springboard.plist`:
- `BKNoWatchdogs = true`
- `FBLaunchWatchdogScale = 20`

### 3. Continuous DECON DMA Scanout
Google Tensor G2 displays will shut off or freeze after bootloader handoff unless the Exynos DECON display controller is programmed for continuous DMA refresh. In `loader_main.c`:
```c
mmio_write32(0x1C133060, 0x3061); /* DECON continuous scanout trigger */
```

### 4. Physical DRAM Boundary (320 MB Window)
Tensor G2 firmware reserves memory above `0x104000000` and between `0x80000000` - `0xA0000000` for TrustZone / secure CARVE-OUT. Passing the full 12 GB DRAM map to XNU triggers an immediate secure EL3 panic. Constraining the physical memory window to 320 MB starting at `0xF0000000` ensures stable, panic-free operation.

---

## Contributing & Development Reference

For an in-depth breakdown of MMIO registers, UFS PHY training sequences, GICv3 interrupt routing, and kernel Mach-O hex patches, refer to:
- [`TENSOR_G2_PORTING_MASTER_GUIDE.md`](file:///Users/vladislav/.gemini/antigravity/scratch/hackios-xnu-kernel/TENSOR_G2_PORTING_MASTER_GUIDE.md)

---

## License

This project is licensed under the terms described in the [LICENSE](file:///Users/vladislav/.gemini/antigravity/scratch/hackios-xnu-kernel/LICENSE) file.  
*iOS, iPhone, Apple, SpringBoard, and Darwin are trademarks of Apple Inc. This is an independent research project not affiliated with or endorsed by Apple Inc. or Google LLC.*
