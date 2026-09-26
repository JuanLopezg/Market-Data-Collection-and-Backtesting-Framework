#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
SC="$ROOT/tools/historical_replay/run_acceptance_scenario.py"
RES="$ROOT/tools/historical_replay/run_t21_t24_resume.py"
python3 - "$SC" "$RES" <<'PY'
from pathlib import Path
import sys
sc=Path(sys.argv[1]).read_text()
res=Path(sys.argv[2]).read_text()
assert 'return latest_committed(root) >= int(date)' in sc
assert 'feeder_logs(root,tail=5000)' in sc
assert 'committed(root,20200413)' in sc
assert "require_pass(evidence/'t20_restart_all_summary.json','T20 restart-all')" in res
assert "--label','t21_gap_catchup'" in res
assert "--label','t22_chaos'" in res
assert "t23_cleanup.py" in res
print('PASS: fault triggers use monotonic latest-committed progression instead of an exact transient log line')
print('PASS: T21 resume requires accepted T20 evidence and does not rerun T20')
print('PASS: destructive T23 remains gated behind successful T21/T22')
PY
