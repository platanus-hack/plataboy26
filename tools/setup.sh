#!/bin/sh
# Download the xPack RISC-V GCC toolchain into tools/ (used by games/*/makefile).
# The simulator itself only needs a C compiler and Node; rebuilding the WebAssembly
# core additionally needs LLVM + lld (macOS: brew install llvm lld).
set -e
cd "$(dirname "$0")"
VER=15.2.0-1
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  PLAT=darwin-arm64 ;;
  Darwin-x86_64) PLAT=darwin-x64 ;;
  Linux-x86_64)  PLAT=linux-x64 ;;
  Linux-aarch64) PLAT=linux-arm64 ;;
  *) echo "unsupported platform, install riscv-none-elf-gcc manually"; exit 1 ;;
esac
if [ -d "xpack-riscv-none-elf-gcc-$VER" ]; then echo "toolchain already installed"; exit 0; fi
URL="https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v$VER/xpack-riscv-none-elf-gcc-$VER-$PLAT.tar.gz"
echo "downloading $URL"
curl -fSL "$URL" | tar xz
"./xpack-riscv-none-elf-gcc-$VER/bin/riscv-none-elf-gcc" --version | head -1
