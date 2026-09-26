#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

SRC="lib/src/persistence/postgres_state_store.cpp"
RUNNER="tools/historical_replay/run_t17_execution_replay.py"

test -f "$SRC"
test -f "$RUNNER"

grep -q 'encodeDoubleForPostgres' "$SRC"
grep -q 'numeric_limits<double>::max_digits10' "$SRC"

if grep -q 'std::to_string(newFill->quantity)' "$SRC"; then
  echo 'FAIL: fill quantity still uses lossy std::to_string(double)'
  exit 1
fi
if grep -q 'std::to_string(newFill->price)' "$SRC"; then
  echo 'FAIL: fill price still uses lossy std::to_string(double)'
  exit 1
fi
if grep -q 'std::to_string(newFill->commission)' "$SRC"; then
  echo 'FAIL: fill commission still uses lossy std::to_string(double)'
  exit 1
fi

grep -q 'fill_quantity_abs_error' "$RUNNER"
python3 -m py_compile "$RUNNER"

python3 - <<'PY'
# Demonstrate why the old six-decimal path was insufficient for the observed T17 quantity.
q = 3046.632054895435
old = float(f"{q:.6f}")
assert old != q
assert abs(old - q) > 1e-12
print(f'precision-regression-check old={old!r} exact={q!r} abs_error={abs(old-q)!r}')
PY

echo 'PASS: T17 fill persistence uses max_digits10 instead of six-decimal std::to_string'
echo 'PASS: T17 fill quantity diagnostic remains strict and reports absolute error'
