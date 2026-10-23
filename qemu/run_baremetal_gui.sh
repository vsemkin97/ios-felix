#!/bin/bash
# run_baremetal_gui.sh - Run HackiOS Bare-Metal Loader & XNU in QEMU with Cocoa GUI window
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== Launching HackiOS Bare-Metal XNU in QEMU ==="
qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a76 \
    -smp 1 \
    -m 6G \
    -kernel baremetal_loader/Image \
    -display cocoa \
    -serial stdio \
    -no-reboot "$@"
