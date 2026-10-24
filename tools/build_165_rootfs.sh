#!/bin/bash
# ==============================================================================
# build_165_rootfs.sh - Automated iOS 16.5 (20F66) Userland Setup for Pixel Fold
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

RAW_IMAGE="/Users/vladislav/pixelios-artifacts/ios-root.img"
SPARSE_IMAGE="${RAW_IMAGE%.img}-clean.simg"
MOUNT_POINT="/private/tmp/iosroot_mnt"
IOS165_DIR="/tmp/userland_inspect/ios165"
PREBUILT_DIR="/tmp/userland_inspect/prebuilt/userland"

echo "======================================================================"
echo "      Google Pixel Fold (Tensor G2) - iOS 16.5 (20F66) Userland       "
echo "======================================================================"
echo "[*] Target Raw Image:   ${RAW_IMAGE}"
echo "[*] Sparse Output:      ${SPARSE_IMAGE}"
echo "[*] Mount Point:        ${MOUNT_POINT}"
echo "======================================================================"

if [ ! -d "${IOS165_DIR}/rootfs" ] || [ ! -d "${IOS165_DIR}/cryptex" ] || [ ! -d "${IOS165_DIR}/20F66__iPhone10,3_6" ]; then
    echo "[-] ERROR: iOS 16.5 extracted directories not found in ${IOS165_DIR}" >&2
    exit 1
fi

# Clean up any previous mount
if mount | grep -q "${MOUNT_POINT}"; then
    echo "[*] Unmounting previous mount at ${MOUNT_POINT}..."
    hdiutil detach "${MOUNT_POINT}" -force || true
fi

mkdir -p "${MOUNT_POINT}"
echo "[*] [1/8] Mounting HFS+ rootfs with owner preservation..."
hdiutil attach "${RAW_IMAGE}" -mountpoint "${MOUNT_POINT}" -owners on

cleanup() {
    if mount | grep -q "${MOUNT_POINT}"; then
        echo "[*] Cleaning up: Unmounting ${MOUNT_POINT}..."
        sync
        hdiutil detach "${MOUNT_POINT}" || hdiutil detach "${MOUNT_POINT}" -force || true
    fi
}
trap cleanup EXIT

if [ ! -d "${MOUNT_POINT}/System/Library" ] || [ ! -d "${MOUNT_POINT}/System/Cryptexes/OS" ]; then
    echo "[*] [2/8] Cleaning previous rootfs contents..."
    rm -rf "${MOUNT_POINT}/"* "${MOUNT_POINT}/".[!.]* 2>/dev/null || true

    echo "[*] [3/8] Copying clean iOS 16.5 rootfs (2.9 GB)..."
    cp -a "${IOS165_DIR}/rootfs/"* "${MOUNT_POINT}/"

    echo "[*] [4/8] Installing Cryptexes & dyld shared cache (2.5 GB)..."
    mkdir -p "${MOUNT_POINT}/System/Cryptexes/OS" "${MOUNT_POINT}/System/Cryptexes/App"
    cp -a "${IOS165_DIR}/cryptex/"* "${MOUNT_POINT}/System/Cryptexes/OS/"
    cp -a "${IOS165_DIR}/cryptex-app/"* "${MOUNT_POINT}/System/Cryptexes/App/"

    DYLD_CACHE_DIR="${MOUNT_POINT}/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
    mkdir -p "${DYLD_CACHE_DIR}"
    cp -a "${IOS165_DIR}/20F66__iPhone10,3_6/"* "${DYLD_CACHE_DIR}/"

    # Move staged system apps to Applications if present
    if [ -d "${MOUNT_POINT}/private/var/staged_system_apps" ]; then
        echo "[*] Moving staged system applications to /Applications..."
        mkdir -p "${MOUNT_POINT}/Applications"
        mv "${MOUNT_POINT}/private/var/staged_system_apps/"* "${MOUNT_POINT}/Applications/" 2>/dev/null || true
        rm -rf "${MOUNT_POINT}/private/var/staged_system_apps"
    fi
else
    echo "[*] Rootfs and cryptexes already extracted on image, keeping base files..."
fi

echo "[*] Creating essential root mountpoints and symlinks from system.manifest..."
mkdir -p "${MOUNT_POINT}/dev" "${MOUNT_POINT}/cores" "${MOUNT_POINT}/Developer"
mkdir -p "${MOUNT_POINT}/private/tmp" "${MOUNT_POINT}/private/var/tmp" "${MOUNT_POINT}/private/preboot"
chmod 555 "${MOUNT_POINT}/dev" 2>/dev/null || true
chmod 775 "${MOUNT_POINT}/cores" "${MOUNT_POINT}/Developer" 2>/dev/null || true
ln -sfn private/etc "${MOUNT_POINT}/etc"
ln -sfn private/var "${MOUNT_POINT}/var"
ln -sfn private/var/tmp "${MOUNT_POINT}/tmp"

echo "[*] [5/8] Creating /private/var hierarchy from var.skel..."
python3 - <<EOF
import os

base = "${MOUNT_POINT}"
var_skel = "${IOS165_DIR}/var.skel"

if os.path.exists(var_skel):
    with open(var_skel, 'r') as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) >= 5 and parts[0] == 'd':
                mode = int(parts[1], 8)
                uid = int(parts[2])
                gid = int(parts[3])
                rel_path = parts[4].lstrip('/')
                full_path = os.path.join(base, rel_path)
                try:
                    os.makedirs(full_path, exist_ok=True)
                    os.chmod(full_path, mode)
                except Exception:
                    pass
                try:
                    os.chown(full_path, uid, gid)
                except Exception:
                    pass
    print("    [+] /private/var hierarchy created successfully.")
EOF

echo "[*] [6/8] Injecting genuine libredwood hooks into /usr/lib/..."
mkdir -p "${MOUNT_POINT}/usr/lib"
cp -v "${SCRIPT_DIR}/userland_patches/lib/libredwood"*.dylib "${MOUNT_POINT}/usr/lib/"
chmod 755 "${MOUNT_POINT}/usr/lib/libredwood"*.dylib

echo "[*] [7/8] Configuring launchd.plist, essential LaunchDaemons & hooks..."
python3 - <<EOF
import plistlib
import os
import shutil

base = "${MOUNT_POINT}"
xpc_dir = os.path.join(base, "System/Library/xpc")
ld_dir = os.path.join(base, "System/Library/LaunchDaemons")
backup_dir = os.path.join(base, "System/Library/LaunchDaemons.stock_disabled")
os.makedirs(backup_dir, exist_ok=True)

# Canonical 21 essential jobs from hackios build.sh
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

# 1. Update /System/Library/xpc/launchd.plist
plist_path = os.path.join(xpc_dir, "launchd.plist")
if os.path.exists(plist_path):
    with open(plist_path, 'rb') as f:
        catalog = plistlib.load(f)
    
    filtered_lds = {}
    for k, v in catalog.get('LaunchDaemons', {}).items():
        basename = os.path.basename(k)
        if basename in essential_jobs:
            v['StandardErrorPath'] = '/dev/console'
            v['StandardOutPath'] = '/dev/console'
            v.pop('_LimitLoadToDeviceTree', None)
            
            # Hooks injection
            if basename == 'com.apple.SpringBoard.plist':
                v.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
            elif basename in ('com.apple.backboardd.plist', 'com.apple.containermanagerd.plist', 'com.apple.containermanagerd.system.plist'):
                v.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
                
            filtered_lds[k] = v
            
    catalog['LaunchDaemons'] = filtered_lds
    with open(plist_path, 'wb') as f:
        plistlib.dump(catalog, f, fmt=plistlib.FMT_BINARY)
    print(f"    [+] Updated {plist_path} (filtered to {len(filtered_lds)} jobs, binary format)")

# 2. Prune and configure LaunchDaemons on disk
for f in os.listdir(ld_dir):
    if not f.endswith(".plist"):
        continue
    if f not in essential_jobs:
        shutil.move(os.path.join(ld_dir, f), os.path.join(backup_dir, f))

for f in essential_jobs:
    p = os.path.join(ld_dir, f)
    if os.path.exists(p):
        with open(p, 'rb') as fp:
            job = plistlib.load(fp)
        job['StandardErrorPath'] = '/dev/console'
        job['StandardOutPath'] = '/dev/console'
        job.pop('_LimitLoadToDeviceTree', None)
        
        if f == 'com.apple.SpringBoard.plist':
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood-sb.dylib'
        elif f in ('com.apple.backboardd.plist', 'com.apple.containermanagerd.plist', 'com.apple.containermanagerd.system.plist'):
            job.setdefault('EnvironmentVariables', {})['DYLD_INSERT_LIBRARIES'] = '/usr/lib/libredwood.dylib'
            
        with open(p, 'wb') as fp:
            plistlib.dump(job, fp, fmt=plistlib.FMT_XML)
            
print(f"    [+] Successfully configured {len(essential_jobs)} essential jobs on disk with hooks.")
EOF

# Install preferences (SetupDone = true, migration = 20F66)
PREF_DIR="${MOUNT_POINT}/private/var/mobile/Library/Preferences"
ROOT_PREF_DIR="${MOUNT_POINT}/private/var/root/Library/Preferences"
mkdir -p "${PREF_DIR}" "${ROOT_PREF_DIR}"
cp -v "${SCRIPT_DIR}/userland_patches/preferences/"*.plist "${PREF_DIR}/"
cp -v "${SCRIPT_DIR}/userland_patches/preferences/com.apple.migration.plist" "${ROOT_PREF_DIR}/"
chown -R 501:501 "${PREF_DIR}" 2>/dev/null || true
chmod 644 "${PREF_DIR}"/*.plist "${ROOT_PREF_DIR}"/*.plist 2>/dev/null || true

echo "[*] [8/8] Flushing writes and unmounting..."
sync
hdiutil detach "${MOUNT_POINT}"
trap - EXIT

echo "[*] Converting to Android sparse image (${SPARSE_IMAGE})..."
python3 "${SCRIPT_DIR}/make-android-sparse.py" "${RAW_IMAGE}" "${SPARSE_IMAGE}"

echo ""
echo "======================================================================"
echo "[+] CLEAN iOS 16.5 USERLAND READY FOR FLASHING"
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
