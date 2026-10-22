#!/bin/bash
# Boot the XNU loader + kernel payload under QEMU (virt, GICv3, UEFI at EL1).
# usage: run.sh [seconds] [extra qemu args...]
# env: QEMU_TB_MB           megabytes for QEMU's cache of translated code (default 384; QEMU's own
#                           default of 1 GB mostly costs host memory)
#      QEMU_SMP             processor cores of the machine (default 1); the kernel uses as many of them as
#                           its boot-arg cpus=N says (CMDLINE="cpus=4" for run-ios.sh), one without it
#      QEMU_EFI, QEMU_VARS  the UEFI firmware for aarch64 guests (default: where Arch's edk2-aarch64
#                           puts it; Debian/Ubuntu: /usr/share/AAVMF/AAVMF_CODE.fd and AAVMF_VARS.fd)
cd "$(dirname "$0")"
T=${1:-20}; shift
# Find UEFI firmware: check macOS Homebrew first, then standard Linux paths
EFI_DEFAULT=/usr/share/edk2/aarch64/QEMU_EFI.fd
VARS_DEFAULT=/usr/share/edk2/aarch64/QEMU_VARS.fd
if [ -f /opt/homebrew/share/qemu/edk2-aarch64-code.fd ]; then
	EFI_DEFAULT=/opt/homebrew/share/qemu/edk2-aarch64-code.fd
	VARS_DEFAULT=/opt/homebrew/share/qemu/edk2-arm-vars.fd
elif [ -f /usr/share/AAVMF/AAVMF_CODE.fd ]; then
	EFI_DEFAULT=/usr/share/AAVMF/AAVMF_CODE.fd
	VARS_DEFAULT=/usr/share/AAVMF/AAVMF_VARS.fd
fi
EFI=${QEMU_EFI:-$EFI_DEFAULT}
[ -f "$EFI" ] || { echo "run.sh: no UEFI firmware at $EFI (set QEMU_EFI and QEMU_VARS)"; exit 1; }
cp -f "${QEMU_VARS:-$VARS_DEFAULT}" vars.fd 2>/dev/null
# the loader is the boot application of the little FAT volume QEMU makes of esp/:
# the one built here, or else the one that comes with the repository
L=../xnuloader/loader.efi; [ -f $L ] || L=../prebuilt/loader.efi
mkdir -p esp/EFI/BOOT && cp -f $L esp/EFI/BOOT/BOOTAA64.EFI
DISP=${QEMU_DISPLAY:-cocoa}
# Kill any stale QEMU process occupying port 4444
killall qemu-system-aarch64 2>/dev/null || true
sleep 0.5

if [ "$T" = "0" ] || [ "$T" = "gui" ] || [ "$T" = "inf" ]; then
	TIMEOUT_CMD=()
else
	TIMEOUT_CMD=(timeout "$T")
fi

"${TIMEOUT_CMD[@]}" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp ${QEMU_SMP:-1} -m ${QEMU_MEM:-6G} \
  -accel tcg,tb-size=${QEMU_TB_MB:-384} \
  -drive if=pflash,format=raw,readonly=on,file="$EFI" \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=fat:rw:esp,format=raw,if=virtio \
  -device loader,file=payload.bin,addr=0xF0000000,force-raw=on \
  -device ramfb \
  -display "$DISP" -serial stdio \
  -monitor tcp:127.0.0.1:4444,server,nowait \
  -no-reboot "$@" 2>&1
