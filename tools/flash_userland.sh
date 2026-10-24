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

echo "[*] [3/6] Applying filtered launchd.plist & pruned LaunchDaemons..."
# Restore NanoLaunchDaemons / LaunchAngels if disabled
if [ -d "${MOUNT_POINT}/System/Library/NanoLaunchDaemons.disabled" ]; then
    mv -v "${MOUNT_POINT}/System/Library/NanoLaunchDaemons.disabled" "${MOUNT_POINT}/System/Library/NanoLaunchDaemons"
fi
if [ -d "${MOUNT_POINT}/System/Library/LaunchAngels.disabled" ]; then
    mv -v "${MOUNT_POINT}/System/Library/LaunchAngels.disabled" "${MOUNT_POINT}/System/Library/LaunchAngels"
fi

# 1. Update /System/Library/xpc/launchd.plist
# Crucial discovery: launchd_cache_loader requires launchd.plist to exist, otherwise it aborts and panics.
# Because boot-args has launchd_unsecure_cache=1, it parses launchd.plist without signature checks.
# By filtering its LaunchDaemons catalog to only our 21 essential jobs, launchd_cache_loader succeeds
# AND loads ONLY the 21 essential jobs into launchd's runtime cache!
python3 - <<EOF
import plistlib
import os
import shutil

base = "${MOUNT_POINT}"
xpc_dir = os.path.join(base, "System/Library/xpc")
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

# 1. Filter /System/Library/xpc/launchd.plist
plist_orig = os.path.join(xpc_dir, "launchd.plist.orig")
plist_path = os.path.join(xpc_dir, "launchd.plist")
plist_disabled = os.path.join(xpc_dir, "launchd.plist.disabled")

if os.path.exists(plist_disabled) and not os.path.exists(plist_orig):
    shutil.copyfile(plist_disabled, plist_orig)
elif os.path.exists(plist_path) and not os.path.exists(plist_orig):
    shutil.copyfile(plist_path, plist_orig)

source_plist = plist_orig if os.path.exists(plist_orig) else (plist_disabled if os.path.exists(plist_disabled) else plist_path)

with open(source_plist, 'rb') as f:
    catalog = plistlib.load(f)

filtered_lds = {}
for k, v in catalog.get('LaunchDaemons', {}).items():
    basename = os.path.basename(k)
    if basename in essential_jobs:
        v['StandardErrorPath'] = '/dev/console'
        v['StandardOutPath'] = '/dev/console'
        v.pop('_LimitLoadToDeviceTree', None)
        if basename == 'com.apple.SpringBoard.plist':
            v.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
        elif basename == 'com.apple.backboardd.plist':
            v.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
        filtered_lds[k] = v

catalog['LaunchDaemons'] = filtered_lds

with open(plist_path, 'wb') as f:
    plistlib.dump(catalog, f, fmt=plistlib.FMT_BINARY)
print(f"    [+] Updated {plist_path} (filtered to {len(filtered_lds)} jobs, binary format)")

# Clean up disabled file if exists
if os.path.exists(plist_disabled) and os.path.exists(plist_path):
    os.remove(plist_disabled)

# 2. Synchronize /System/Library/LaunchDaemons on disk
for d in [ld_dir, backup_dir]:
    if os.path.isdir(d):
        for f in os.listdir(d):
            clean_name = f.replace(".disabled", "")
            if clean_name in essential_jobs:
                src_path = os.path.join(d, f)
                dst_path = os.path.join(ld_dir, clean_name)
                if src_path != dst_path:
                    shutil.move(src_path, dst_path)

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

print(f"    [+] Kept {len(kept)} essential launchd jobs in LaunchDaemons on disk.")
print(f"    [+] Stashed {stashed} non-essential background daemons to LaunchDaemons.stock_disabled.")

# Configure the kept essential jobs
for f in kept:
    p = os.path.join(ld_dir, f)
    try:
        with open(p, 'rb') as fp:
            job = plistlib.load(fp)
        
        job['StandardErrorPath'] = '/dev/console'
        job['StandardOutPath'] = '/dev/console'
        job.pop('_LimitLoadToDeviceTree', None)
        
        if f == "com.apple.SpringBoard.plist":
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
        elif f == "com.apple.backboardd.plist":
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
            
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
