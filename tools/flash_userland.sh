#!/bin/bash
# ==============================================================================
# flash_userland.sh - Automated iOS Userland Patching & Flashing for Pixel Fold
# ==============================================================================
# This script mounts a raw HFS+ iOS rootfs image, applies all required patches
# (universal libredwood-sb injection, stubbed mobileactivationd, disabled
# crash daemons, build 20H392 migration records, SetupDone preferences),
# converts the raw image into an Android sparse image (.simg), and flashes it
# directly to the physical UFS 'userdata' partition via fastboot.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEFAULT_RAW_IMAGE="/Users/vladislav/pixelios-artifacts/ios-root.img"
RAW_IMAGE="${1:-$DEFAULT_RAW_IMAGE}"
MOUNT_POINT="/private/tmp/iosroot_mnt"
SPARSE_IMAGE="${RAW_IMAGE%.img}-clean.simg"

echo "======================================================================"
echo "          Google Pixel Fold (Tensor G2) - iOS Userland Setup          "
echo "======================================================================"
echo "[*] Target Raw Image:   ${RAW_IMAGE}"
echo "[*] Sparse Output:      ${SPARSE_IMAGE}"
echo "[*] Mount Point:        ${MOUNT_POINT}"
echo "======================================================================"

if [ ! -f "${RAW_IMAGE}" ]; then
    echo "[-] ERROR: Source raw image not found at '${RAW_IMAGE}'" >&2
    echo "    Usage: $0 [path/to/ios-root.img]" >&2
    exit 1
fi

# Clean up any previous mount
if mount | grep -q "${MOUNT_POINT}"; then
    echo "[*] Unmounting previous mount at ${MOUNT_POINT}..."
    hdiutil detach "${MOUNT_POINT}" -force || true
fi

mkdir -p "${MOUNT_POINT}"

echo "[*] [1/6] Mounting HFS+ rootfs with owner preservation..."
hdiutil attach "${RAW_IMAGE}" -mountpoint "${MOUNT_POINT}" -owners on

cleanup() {
    if mount | grep -q "${MOUNT_POINT}"; then
        echo "[*] Cleaning up: Unmounting ${MOUNT_POINT}..."
        sync
        hdiutil detach "${MOUNT_POINT}" || hdiutil detach "${MOUNT_POINT}" -force || true
    fi
}
trap cleanup EXIT

echo "[*] [2/6] Injecting universal libredwood dylibs into /usr/lib/..."
mkdir -p "${MOUNT_POINT}/usr/lib"
cp -v "${SCRIPT_DIR}/userland_patches/lib/libredwood-sb.dylib" "${MOUNT_POINT}/usr/lib/"
cp -v "${SCRIPT_DIR}/userland_patches/lib/libredwood.dylib" "${MOUNT_POINT}/usr/lib/"
chmod 755 "${MOUNT_POINT}/usr/lib/libredwood-sb.dylib" "${MOUNT_POINT}/usr/lib/libredwood.dylib"

echo "[*] [3/6] Applying upstream hackios launchd architecture..."
# 1. Disable /System/Library/xpc/launchd.plist
# Crucial discovery: launchd.plist is Apple's embedded catalog of ~523 jobs.
# If launchd.plist exists, launchd loads all 523 jobs and completely ignores LaunchDaemons!
if [ -f "${MOUNT_POINT}/System/Library/xpc/launchd.plist" ]; then
    echo "    [+] Disabling /System/Library/xpc/launchd.plist (forces launchd to read LaunchDaemons/)..."
    mv -v "${MOUNT_POINT}/System/Library/xpc/launchd.plist" "${MOUNT_POINT}/System/Library/xpc/launchd.plist.disabled"
fi

# 2. Disable NanoLaunchDaemons and LaunchAngels
if [ -d "${MOUNT_POINT}/System/Library/NanoLaunchDaemons" ]; then
    echo "    [+] Disabling /System/Library/NanoLaunchDaemons..."
    mv -v "${MOUNT_POINT}/System/Library/NanoLaunchDaemons" "${MOUNT_POINT}/System/Library/NanoLaunchDaemons.disabled"
fi
if [ -d "${MOUNT_POINT}/System/Library/LaunchAngels" ]; then
    echo "    [+] Disabling /System/Library/LaunchAngels..."
    mv -v "${MOUNT_POINT}/System/Library/LaunchAngels" "${MOUNT_POINT}/System/Library/LaunchAngels.disabled"
fi

# 3. Clean and prune LaunchDaemons to only essential IOS_JOBS
python3 - <<EOF
import plistlib
import os
import shutil

base = "${MOUNT_POINT}"
ld_dir = os.path.join(base, "System/Library/LaunchDaemons")
backup_dir = os.path.join(base, "System/Library/LaunchDaemons.stock_disabled")
os.makedirs(backup_dir, exist_ok=True)

# Upstream canonical IOS_JOBS from hackios-xnu-userland (build.sh lines 99-107)
essential_jobs = {
    "com.apple.notifyd.plist",
    "com.apple.cfprefsd.xpc.daemon.plist",
    "com.apple.cfprefsd.xpc.daemon.system.plist",
    "com.apple.logd.plist",
    "com.apple.syslogd.plist",
    "com.apple.distnoted.xpc.daemon.plist",
    "com.apple.diagnosticd.plist",
    "com.apple.fseventsd.plist",
    "com.apple.containermanagerd.plist",
    "com.apple.containermanagerd.system.plist",
    "com.apple.runningboardd.plist",
    "com.apple.backboardd.plist",
    "com.apple.mobile.usermanagerd.plist",
    "com.apple.mobilegestalt.xpc.plist",
    "com.apple.lsd.plist",
    "com.apple.powerd.plist",
    "com.apple.analyticsd.plist",
    "com.apple.logd_helper.plist",
    "com.apple.iconservices.iconservicesagent.plist",
    "com.apple.pluginkit.pkd.plist",
    "com.apple.SpringBoard.plist",
}

# Restore any essential plists that might have been moved to backup or disabled
for d in [ld_dir, backup_dir]:
    if os.path.isdir(d):
        for f in os.listdir(d):
            clean_name = f.replace(".disabled", "")
            if clean_name in essential_jobs:
                src_path = os.path.join(d, f)
                dst_path = os.path.join(ld_dir, clean_name)
                if src_path != dst_path:
                    shutil.move(src_path, dst_path)

# Move all non-essential plists to backup_dir
kept = []
stashed = 0
for f in os.listdir(ld_dir):
    if not f.endswith(".plist"):
        continue
    if f in essential_jobs:
        kept.append(f)
    else:
        shutil.move(os.path.join(ld_dir, f), os.path.join(backup_dir, f))
        stashed += 1

print(f"    [+] Kept {len(kept)} essential launchd jobs in LaunchDaemons.")
print(f"    [+] Stashed {stashed} non-essential background daemons to LaunchDaemons.stock_disabled.")

# Configure the kept essential jobs
for f in kept:
    p = os.path.join(ld_dir, f)
    try:
        with open(p, 'rb') as fp:
            job = plistlib.load(fp)
        
        # Route standard streams to /dev/console
        job['StandardErrorPath'] = '/dev/console'
        job['StandardOutPath'] = '/dev/console'
        job.pop('_LimitLoadToDeviceTree', None)
        
        # SpringBoard injection
        if f == "com.apple.SpringBoard.plist":
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
            print("    [+] Patched com.apple.SpringBoard.plist (DYLD_INSERT_LIBRARIES -> libredwood-sb.dylib)")
            
        # backboardd injection
        if f == "com.apple.backboardd.plist":
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
            print("    [+] Patched com.apple.backboardd.plist (DYLD_INSERT_LIBRARIES -> libredwood.dylib)")
            
        with open(p, 'wb') as fp:
            plistlib.dump(job, fp, fmt=plistlib.FMT_XML)
    except Exception as e:
        print(f"    [-] Error patching {f}: {e}")
EOF

echo "[*] [4/6] Installing SetupDone and build 20H392 migration preferences..."
PREF_DIR="${MOUNT_POINT}/private/var/mobile/Library/Preferences"
ROOT_PREF_DIR="${MOUNT_POINT}/private/var/root/Library/Preferences"
mkdir -p "${PREF_DIR}" "${ROOT_PREF_DIR}"
cp -v "${SCRIPT_DIR}/userland_patches/preferences/"*.plist "${PREF_DIR}/"
cp -v "${SCRIPT_DIR}/userland_patches/preferences/com.apple.migration.plist" "${ROOT_PREF_DIR}/"
chown -R 501:501 "${PREF_DIR}"
chmod 644 "${PREF_DIR}"/*.plist "${ROOT_PREF_DIR}"/*.plist

echo "[*] [5/6] Flushing writes and detaching disk image..."
sync
hdiutil detach "${MOUNT_POINT}"
trap - EXIT

echo "[*] [6/6] Converting to Android sparse image (${SPARSE_IMAGE})..."
python3 "${SCRIPT_DIR}/make-android-sparse.py" "${RAW_IMAGE}" "${SPARSE_IMAGE}"

echo ""
echo "======================================================================"
echo "[+] USERLAND IMAGE READY FOR FLASHING"
echo "======================================================================"
echo "[*] Checking fastboot connection..."
if fastboot devices | grep -q "fastboot"; then
    echo "[+] Fastboot device detected!"
    echo "[*] Flashing sparse userland to 'userdata' partition..."
    fastboot flash userdata "${SPARSE_IMAGE}"
    echo "[+] Userland successfully flashed!"

    LOADER_IMG="${REPO_DIR}/baremetal_loader/felix_xnu_boot_v4.img"
    if [ -f "${LOADER_IMG}" ]; then
        SHOULD_BOOT=true
        if [ -t 0 ]; then
            read -p "[?] Would you like to boot XNU now? (Y/n): " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Nn]$ ]]; then
                SHOULD_BOOT=false
            fi
        fi
        if [ "${SHOULD_BOOT}" = true ]; then
            echo "[*] Booting ${LOADER_IMG}..."
            fastboot boot "${LOADER_IMG}"
        fi
    fi
else
    echo "[-] WARNING: No device in fastboot mode found."
    echo "    To flash manually when device is in bootloader:"
    echo "    $ fastboot flash userdata ${SPARSE_IMAGE}"
    echo "    $ fastboot boot baremetal_loader/felix_xnu_boot_v4.img"
fi

echo "======================================================================"
echo "[*] All steps completed successfully!"
echo "======================================================================"
