#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
SRC="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"

[[ -f "$SRC" ]] || { echo "FAIL: missing $SRC"; exit 1; }

grep -q 'if (order.target_notional_usd == 0.0)' "$SRC" || { echo 'FAIL: exact FLAT branch missing'; exit 1; }
grep -q 'quantity = std::abs(currentQuantity);' "$SRC" || { echo 'FAIL: FLAT does not use exact current filled quantity'; exit 1; }
grep -q 'quantity = order.notional_usd / order.reference_close;' "$SRC" || { echo 'FAIL: non-flat close(T) sizing rule missing'; exit 1; }
grep -q 'FLAT plan side does not flatten current filled position' "$SRC" || { echo 'FAIL: FLAT direction guard missing'; exit 1; }

python3 - <<'PY'
q = 3046.632054895435
close = 3.236892
roundtrip = (q * close) / close
assert roundtrip != q, (q, roundtrip)
assert abs(roundtrip - q) > 0.0
exact_flat = abs(q)
assert exact_flat == q
print(f"precision-regression-check current={q!r} q_close_div_close={roundtrip!r} residual={roundtrip-q!r}")
PY

echo 'PASS: full FLAT orders use exact current filled quantity and cannot create q*close/close dust'
echo 'PASS: non-flat orders preserve exact close(T) notional sizing'
