#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

echo "============================================================"
echo "T17-T24 — CUMULATIVE PREFLIGHT"
echo "============================================================"

bash validation/t17_execution_replay_audit.sh .
bash validation/t17_fill_precision_hotfix_audit.sh .
bash validation/t18_bounded_fingerprint_audit.sh .
python3 -m py_compile \
  tools/historical_replay/run_acceptance_scenario.py \
  tools/historical_replay/run_t17_t24.py \
  tools/historical_replay/t23_cleanup.py
python3 tools/historical_replay/t23_cleanup.py --root . >/tmp/t23_cleanup_plan.json

python3 - <<'PY'
from pathlib import Path
import json
s=Path('tools/historical_replay/run_t17_t24.py').read_text()
r=Path('tools/historical_replay/run_acceptance_scenario.py').read_text()
# User-approved speed policy: no day-scale 0.5x/1x/2x/10x/100x campaign.
assert "'1500'" in s and "'1000'" in s and "'10000'" in s
for forbidden in ['0.5','1','2','10','100']:
    needle="'--speed','"+forbidden+"'"
    assert needle not in s, f'forbidden slow acceptance speed found: {forbidden}x'
assert 'FULL_TARGET = 20251012' in r
assert "default=10000.0" in s
assert "t19a_capacity_10000" in s
assert "--capacity-observation-start','20200420'" in s
assert "'--target-completed','20200510'" in s
assert "'--capacity-min-span-days','10'" in s
assert "--capacity-max-lag-days','2'" in s
assert "--max-wall-seconds','24000'" in s
assert "--capacity-check" in r
assert "capacity_max_lag_days_observed" in r
assert 'bounded_economic_state' in r
assert 'target_execution_day' in r
assert 'fingerprint(root,args.target_completed,exec_day)' in r
assert 'FULL_HISTORY_STRUCTURAL_BASELINE' in r
assert '"strategy_days": 2112' in r
assert '"closed_trades": 630' in r
assert '"open_campaigns": 1' in r
assert 'economic_equivalence_gate' in r
assert 'T19c exact research RealTest policy' in r
assert "choices=['none','restart-all','strategy-gap','chaos']" in r
assert 'fingerprint_sha256' in r
assert 't23_cleanup.py' in s and 't23_no_legacy_clock_audit.sh' in s
assert 't24_post_cleanup' in s
plan=json.loads(Path('/tmp/t23_cleanup_plan.json').read_text())
assert 'live_trading/replay_controller' in plan['delete_dirs']
assert 'deploy/distributed_replay' in plan['delete_dirs']
assert 'lib/src/runtime/service_clock.h' in plan['delete_files']
print('PASS: T18-T24 campaign structure, bounded T18 fingerprint, 10000x capacity gate and T19 structural/RealTest gate split')
PY

bash validation/t18_noop_plan_fingerprint_audit.sh .
bash validation/t19_capacity_steady_state_audit.sh .
bash validation/t19_realtest_gate_audit.sh .

echo "PASS: T17-T24 cumulative preflight"
