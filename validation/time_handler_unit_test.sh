#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
cd "$ROOT"

CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/algotrading_time_handler_unit_test"

"$CXX" \
  -std=c++20 \
  -O2 \
  -Wall -Wextra -Wpedantic \
  -pthread \
  -Ilib/src/runtime \
  validation/time_handler_unit_test.cpp \
  lib/src/runtime/time_handler.cpp \
  -o "$OUT"

"$OUT"
