#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/riscv64}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/riscv64-musl-zig.cmake" \
  -DNANOKVM_RDP_BUILD_TESTS=OFF \
  -DNANOKVM_RDP_BUILD_SERVER=OFF
cmake --build "$BUILD_DIR" --target nanokvm-agent --parallel 4
