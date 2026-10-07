#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
ROOT="$(cd "$ROOT" && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/algotrading_time_handler_factory_test"
BIN="$BUILD_DIR/time_handler_factory_unit_test"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT/lib/src/utils" \
  "$ROOT/validation/time_handler_factory_unit_test.cpp" \
  "$ROOT/lib/src/utils/time_handler.cpp" \
  "$ROOT/lib/src/utils/time_handler_factory.cpp" \
  -pthread \
  -o "$BIN"

"$BIN"
