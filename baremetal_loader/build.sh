#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

CLANG="/opt/homebrew/opt/llvm/bin/clang -target aarch64-none-elf"
LLD="/opt/homebrew/bin/ld.lld"
OBJCOPY="/opt/homebrew/opt/llvm/bin/llvm-objcopy"
NM="/opt/homebrew/opt/llvm/bin/llvm-nm"
READELF="/opt/homebrew/opt/llvm/bin/llvm-readelf"
KERNEL="../prebuilt/mach.development.bcm2837"

echo "=== [0/6] Compiling Userspace Test Launchd Binary ==="
clang -target arm64-apple-macos -nostdlib -static -fno-stack-protector -e __start test_init.c -o test_launchd.bin

echo "=== [1/6] Embedding XNU Kernel Mach-O & Launchd ==="
cp "$KERNEL" kernel.macho
$OBJCOPY -I binary -O elf64-littleaarch64 -B aarch64 kernel.macho kernel_blob.o
$OBJCOPY -I binary -O elf64-littleaarch64 -B aarch64 test_launchd.bin launchd_blob.o

echo "=== [2/6] Compiling Assembly Sources ==="
$CLANG -c entry.S -o entry.o

echo "=== [3/6] Compiling C Sources (strictly 0 ABS64 & 0 GOT relocations) ==="
CFLAGS="-O2 -ffreestanding -nostdlib -fpie -fvisibility=hidden -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -I."
$CLANG $CFLAGS -c console.c -o console.o
$CLANG $CFLAGS -c ufsinit.c -o ufsinit.o
$CLANG $CFLAGS -c loader_main.c -o loader_main.o

abs=$($READELF -r console.o ufsinit.o loader_main.o | grep -c ABS64 || true)
got=$($READELF -r console.o ufsinit.o loader_main.o | grep -c GOT || true)
[ "$abs" = 0 ] || { echo "ERROR: $abs absolute relocations in C objects!" >&2; exit 1; }
[ "$got" = 0 ] || { echo "ERROR: $got GOT relocations in C objects!" >&2; exit 1; }
echo "Verified: strictly 0 ABS64 and 0 GOT relocations!"

echo "=== [4/6] Linking baremetal_loader.elf ==="
$LLD -T linker.ld --build-id=none -N entry.o console.o ufsinit.o loader_main.o kernel_blob.o launchd_blob.o -o baremetal_loader.elf
$OBJCOPY -O binary baremetal_loader.elf Image
size=$($NM baremetal_loader.elf | awk '$3=="_end"{print "0x"$1}')
truncate -s $(printf "%d" "$size") Image

echo "=== [5/6] Compressing with LZ4 (Legacy format) ==="
lz4 -l -12 -f Image Image.lz4

echo "=== [6/6] Packaging Header v4 Boot Image & Adding AVB Signature ==="
python3 ../prebuilt/uefi/mkbootimg.py \
  --header_version 4 \
  --kernel Image.lz4 \
  -o felix_xnu_boot_v4.img

python3 avbtool.py add_hash_footer \
  --image felix_xnu_boot_v4.img \
  --partition_name boot \
  --partition_size 67108864 \
  --algorithm SHA256_RSA4096 \
  --key test_rsa4096.pem \
  --rollback_index 1788566400

ls -lh Image Image.lz4 felix_xnu_boot_v4.img
echo "=== BUILD SUCCESSFUL: felix_xnu_boot_v4.img ready for 'fastboot boot' ==="
