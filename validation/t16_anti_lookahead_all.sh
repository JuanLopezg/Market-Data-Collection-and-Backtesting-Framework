#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"

"$ROOT/validation/historical_visibility_no_lookahead_test.sh" "$ROOT"
"$ROOT/validation/canonical_market_data_no_lookahead_test.sh" "$ROOT"
"$ROOT/validation/distributed_anti_lookahead_audit.sh" "$ROOT"

echo "PASS: T16 anti-lookahead acceptance"
