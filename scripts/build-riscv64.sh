#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
FREERDP_DIR="${FREERDP_DIR:-/Users/yangs/project/fold-vnc/extern/FreeRDP}"
OPENSSL_ROOT="${OPENSSL_ROOT:-$ROOT/build/openssl-riscv64}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/riscv64}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/riscv64-musl-zig.cmake" \
  -DNANOKVM_RDP_BUILD_TESTS=OFF \
  -DNANOKVM_RDP_FREERDP_DIR="$FREERDP_DIR" \
  -DNANOKVM_RDP_OPENSSL_ROOT="$OPENSSL_ROOT" \
  -DWITH_VERBOSE_WINPR_ASSERT=OFF
cmake --build "$BUILD_DIR" --target nanokvm-rdp --parallel 4
