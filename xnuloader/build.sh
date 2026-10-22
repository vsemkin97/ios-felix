#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
CLANG="clang"
LLD_LINK="lld-link"
if [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then
  CLANG="/opt/homebrew/opt/llvm/bin/clang"
fi
if [ -x "/opt/homebrew/bin/lld-link" ]; then
  LLD_LINK="/opt/homebrew/bin/lld-link"
fi

$CLANG --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
$LLD_LINK /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
