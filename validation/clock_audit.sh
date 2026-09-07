#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

python3 "$SCRIPT_DIR/clock_audit.py" \
  --root "$ROOT" \
  --report "validation/CLOCK_AUDIT.md" \
  "$@"
