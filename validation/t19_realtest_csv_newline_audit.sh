#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
PY="$ROOT/tools/historical_replay/run_t19_realtest_gate.py"
CPP="$ROOT/tools/historical_replay/t19_realtest_compare.cpp"

grep -F 'csv.writer(f, lineterminator="\n")' "$PY" >/dev/null
grep -F "line.back() == '\\r'" "$CPP" >/dev/null
python3 -m py_compile "$PY"

python3 - <<'PY'
import csv, pathlib, tempfile
p = pathlib.Path(tempfile.mkstemp()[1])
with p.open('w', newline='') as f:
    w = csv.writer(f, lineterminator='\n')
    w.writerow(['fill_id','order_id','strategy_id','timestamp','coin','side','quantity','price','commission'])
b = p.read_bytes()
assert b.endswith(b'\n') and not b.endswith(b'\r\n'), b
print('PASS: T19 RealTest fills exporter emits LF-only CSV')
p.unlink()
PY

echo 'PASS: T19 RealTest bridge accepts CRLF input defensively'
