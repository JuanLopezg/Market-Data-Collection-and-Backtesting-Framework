#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"
python3 - <<'PY'
from pathlib import Path
import importlib.util
p=Path('tools/historical_replay/run_acceptance_scenario.py')
s=p.read_text()
assert 'ALGOTRADING_TIME_REAL_REFERENCE_UTC' in s
assert 'ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC' in s
assert 'def capacity_observation' in s
assert "'lag_days':max(0,raw_lag)" in s
assert '--capacity-min-span-days' in s
spec=importlib.util.spec_from_file_location('runner',p)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
base={'strategy_latest':0,'risk_latest':0,'planner_latest':0,'execution_last_decision':0}
assert m.capacity_observation(20200422,base,20200420) is None
warm={'strategy_latest':20200419,'risk_latest':20200420,'planner_latest':20200420,'execution_last_decision':20200420}
assert m.capacity_observation(20200422,warm,20200420) is None
steady={'strategy_latest':20200421,'risk_latest':20200421,'planner_latest':20200421,'execution_last_decision':20200421}
x=m.capacity_observation(20200422,steady,20200420)
assert x['raw_lag_days']==1 and x['lag_days']==1
# Pipeline ahead is not negative lag and must not poison max-lag.
ahead={'strategy_latest':20200425,'risk_latest':20200425,'planner_latest':20200425,'execution_last_decision':20200425}
x=m.capacity_observation(20200422,ahead,20200420)
assert x['raw_lag_days']==-3 and x['lag_days']==0
print('PASS: T19 capacity ignores bootstrap samples until all decision stages reach observation_start')
print('PASS: T19 capacity clamps pipeline-ahead samples to zero lag')
print('PASS: T19 capacity reference is derived from immutable TimeHandler env, not bounded feeder log tails')
PY
python3 - <<'PY'
from pathlib import Path
s=Path('tools/historical_replay/run_t17_t24.py').read_text()
assert "'--target-completed','20200510'" in s
assert "'--capacity-min-span-days','10'" in s
assert "'--capacity-max-lag-days','2'" in s
assert "'--speed','10000'" in s
print('PASS: T19a requires >=10 simulated steady-state days at 10000x with <=2-day lag')
PY
