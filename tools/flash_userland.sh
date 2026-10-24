#!/bin/bash
# ==============================================================================
# flash_userland.sh - Automated iOS Userland Patching & Flashing for Pixel Fold
# ==============================================================================
# This script mounts a raw HFS+ iOS rootfs image, applies all required patches
# (libredwood dylib injections, disabled crash daemons, SetupDone preferences),
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

echo "[*] [2/6] Injecting redwood dylibs into /usr/lib/..."
mkdir -p "${MOUNT_POINT}/usr/lib"
cp -v "${SCRIPT_DIR}/userland_patches/lib/libredwood-sb.dylib" "${MOUNT_POINT}/usr/lib/"
cp -v "${SCRIPT_DIR}/userland_patches/lib/libredwood.dylib" "${MOUNT_POINT}/usr/lib/"
chmod 755 "${MOUNT_POINT}/usr/lib/libredwood-sb.dylib" "${MOUNT_POINT}/usr/lib/libredwood.dylib"

echo "[*] [3/6] Patching LaunchDaemons (SpringBoard, backboardd, mobileactivationd)..."
python3 - <<EOF
import plistlib
import os
import sys

base = "${MOUNT_POINT}"

# 1. SpringBoard: inject libredwood-sb.dylib
sb_path = os.path.join(base, "System/Library/LaunchDaemons/com.apple.SpringBoard.plist")
if os.path.exists(sb_path):
    with open(sb_path, 'rb') as f:
        sb = plistlib.load(f)
    if 'EnvironmentVariables' not in sb:
        sb['EnvironmentVariables'] = {}
    sb['EnvironmentVariables']['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
    with open(sb_path, 'wb') as f:
        plistlib.dump(sb, f, fmt=plistlib.FMT_XML)
    print("    [+] Patched com.apple.SpringBoard.plist (DYLD_INSERT_LIBRARIES)")

# 2. backboardd: inject libredwood.dylib
bb_path = os.path.join(base, "System/Library/LaunchDaemons/com.apple.backboardd.plist")
if os.path.exists(bb_path):
    with open(bb_path, 'rb') as f:
        bb = plistlib.load(f)
    if 'EnvironmentVariables' not in bb:
        bb['EnvironmentVariables'] = {}
    bb['EnvironmentVariables']['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
    with open(bb_path, 'wb') as f:
        plistlib.dump(bb, f, fmt=plistlib.FMT_XML)
    print("    [+] Patched com.apple.backboardd.plist (DYLD_INSERT_LIBRARIES)")

# 3. Disable crashing daemons lacking Apple SEP / Baseband
for daemon in ["com.apple.mobileactivationd.plist", "com.apple.watchdogd.plist"]:
    d_path = os.path.join(base, "System/Library/LaunchDaemons", daemon)
    if os.path.exists(d_path):
        with open(d_path, 'rb') as f:
            d = plistlib.load(f)
        d['Disabled'] = True
        with open(d_path, 'wb') as f:
            plistlib.dump(d, f, fmt=plistlib.FMT_XML)
        print(f"    [+] Disabled {daemon}")
EOF

echo "[*] [4/6] Installing SetupDone and migration preferences into /private/var/mobile/..."
PREF_DIR="${MOUNT_POINT}/private/var/mobile/Library/Preferences"
mkdir -p "${PREF_DIR}"
cp -v "${SCRIPT_DIR}/userland_patches/preferences/"*.plist "${PREF_DIR}/"
chown -R 501:501 "${PREF_DIR}"
chmod 644 "${PREF_DIR}"/*.plist

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
        read -p "[?] Would you like to boot XNU now? (y/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
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
