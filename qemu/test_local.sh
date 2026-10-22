#!/bin/bash
# test_local.sh - Local QEMU test harness for Darwin XNU kernel on macOS
set -euo pipefail
cd "$(dirname "$0")"

EFI="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
VARS="/opt/homebrew/share/qemu/edk2-arm-vars.fd"

if [ ! -f "$EFI" ] || [ ! -f "$VARS" ]; then
    echo "ERROR: EDK2 firmware not found in Homebrew share directory." >&2
    exit 1
fi

cp -f "$VARS" vars.fd 2>/dev/null || true

# Prepare ESP partition with xnuloader
L="../xnuloader/loader.efi"
[ -f "$L" ] || L="../prebuilt/loader.efi"
mkdir -p esp/EFI/BOOT
cp -f "$L" esp/EFI/BOOT/BOOTAA64.EFI

# Ensure payload.bin exists
if [ ! -f payload.bin ]; then
    echo "Building payload.bin..."
    K="../prebuilt/mach.development.bcm2837"
    python3 ../xnuloader/mkpayload.py -o payload.bin kernel="$K" --cmdline "debug=0x8 -v serial=3 cpus=1"
fi

SECS=${1:-10}
echo "=== Launching QEMU AArch64 (timeout: ${SECS}s) ==="
/usr/bin/timeout "$SECS" qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a76 \
    -smp 1 \
    -m 6G \
    -accel tcg,tb-size=384 \
    -drive if=pflash,format=raw,readonly=on,file="$EFI" \
    -drive if=pflash,format=raw,file=vars.fd \
    -drive file=fat:rw:esp,format=raw,if=virtio \
    -device loader,file=payload.bin,addr=0xF0000000,force-raw=on \
    -device ramfb \
    -display none \
    -serial stdio \
    -monitor none \
    -no-reboot || true
echo "=== QEMU run complete ==="
