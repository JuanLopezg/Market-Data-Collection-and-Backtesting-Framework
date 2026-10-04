#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

echo "============================================================"
echo "T19 — REALTEST EXACT-POLICY GATE AUDIT"
echo "============================================================"

required=(
  research/src/common/realtest.cpp
  research/src/realtest.h
  tools/distributed_compare/check_realtest_baseline.py
  tools/distributed_compare/realtest_known_baseline.json
  tools/historical_replay/t19_realtest_compare.cpp
  tools/historical_replay/check_t19_realtest_baseline.py
  tools/historical_replay/run_t19_realtest_gate.py
  tools/historical_replay/realtest_known_baseline_snapshot.json
  storage/backtests/final_tests/pureRSI.csv
  storage/databases/1d_cmc.csv
)
for f in "${required[@]}"; do
  [[ -f "$f" ]] || { echo "FAIL: required T19 RealTest input missing: $f" >&2; exit 1; }
done

python3 -m py_compile \
  tools/historical_replay/check_t19_realtest_baseline.py \
  tools/historical_replay/run_t19_realtest_gate.py

python3 - <<'PY'
from pathlib import Path
import json

canonical=json.loads(Path('tools/distributed_compare/realtest_known_baseline.json').read_text())
snapshot=json.loads(Path('tools/historical_replay/realtest_known_baseline_snapshot.json').read_text())
assert canonical == snapshot, 'installed RealTest known baseline differs from the locked T19 snapshot'

ew=canonical['equal_weight']
assert ew['summary'] == {
    'realtest_trades_checked': 631,
    'backtester_trades_total': 631,
    'fully_matched': 628,
    'different_or_missing': 3,
    'missing_backtester_same_day': 1,
}
mm=ew['mismatches']
assert len(mm) == 4
coins=[]
for item in mm:
    trade=item.get('realtest') or item.get('candidate') or {}
    coins.append(trade.get('coin'))
assert coins == ['BNB','FET','ZEC','FET'], coins
assert mm[0]['comparison_index'] == 502 and mm[0]['kind'] == 'matched_mismatch'
assert mm[1]['comparison_index'] == 520 and mm[1]['kind'] == 'missing_candidate'
assert mm[2]['comparison_index'] == 631 and mm[2]['kind'] == 'matched_mismatch'
assert mm[3]['kind'] == 'unmatched_candidate'

bridge=Path('tools/historical_replay/t19_realtest_compare.cpp').read_text()
gate=Path('tools/historical_replay/run_t19_realtest_gate.py').read_text()
campaign=Path('tools/historical_replay/run_t17_t24.py').read_text()
assert 'compareBacktestBySizing' in bridge
assert 'TradeRecorder' in bridge
assert 'REALTEST_NONINTERACTIVE' in gate and '"1"' in gate
assert 'check_t19_realtest_baseline.py' in gate
assert "'--mode','equal-weight'" in campaign
assert 't19_realtest_ok' in campaign
print('PASS: T19 RealTest baseline locked to exact historical exception identities BNB/FET/ZEC')
PY

echo "PASS: T19 exact research-policy gate inputs and scripts are present"
