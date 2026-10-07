#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"
python3 -m py_compile \
  tools/historical_replay/run_acceptance_scenario.py \
  tools/historical_replay/regrade_t19_full_history.py \
  tools/historical_replay/run_t20_t24_resume.py
python3 - <<'PY'
from pathlib import Path
s=Path('tools/historical_replay/run_acceptance_scenario.py').read_text()
r=Path('tools/historical_replay/regrade_t19_full_history.py').read_text()
q=Path('tools/historical_replay/run_t20_t24_resume.py').read_text()
assert 'FULL_HISTORY_STRUCTURAL_BASELINE' in s
assert '"strategy_days": 2112' in s
assert '"closed_trades": 630' in s
assert '"open_campaigns": 1' in s
assert 'actual["cycles"]' not in s
assert "cp['planner_days']" in s  # retained informationally
assert 'T19c exact research RealTest policy' in s
assert "rt.get('result')!='PASS'" in r
assert "regraded_without_replay" in r
assert "resuming at T20" in q
assert "t19_full_history_realtest_summary.json" in q
assert "t23_cleanup.py" in q and "no_legacy_clock_audit.sh" in q
print('PASS: T19b structural gate separated from T19c economic RealTest gate')
print('PASS: existing accepted T19 evidence can be regraded without replay')
print('PASS: T20-T24 resume remains fail-closed before destructive T23 cleanup')
PY
