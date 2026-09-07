#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

python3 "$SCRIPT_DIR/shared_clock_foundation_check.py" --root "$ROOT"

TMP_BIN="$(mktemp /tmp/algotrading_clock_smoke.XXXXXX)"
trap 'rm -f "$TMP_BIN"' EXIT

g++ -std=c++20 -Wall -Wextra -Werror -pthread \
  -I"$ROOT/lib/src/runtime" \
  "$SCRIPT_DIR/clock_foundation_smoke.cpp" \
  -o "$TMP_BIN"

"$TMP_BIN"

echo "============================================================"
echo "STEP 35B FOUNDATION CHECK: PASS"
echo "Next clock gate: bash validation/shared_clock_followers_suite.sh"
echo "============================================================"
