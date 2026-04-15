#!/usr/bin/env bash
# Local equivalent CI: build and run unit tests (Catch2) without Muduo/ZK.
# Usage:
#   bash scripts/ci_unit_tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build_ci_unit}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKRPC_BUILD_FRAMEWORK=OFF \
  -DKRPC_BUILD_TESTS=ON

cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure -V

