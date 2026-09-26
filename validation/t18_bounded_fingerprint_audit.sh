#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

python3 - <<'PY'
from pathlib import Path
import importlib.util

path=Path('tools/historical_replay/run_acceptance_scenario.py')
spec=importlib.util.spec_from_file_location('scenario',path)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

base_plans=[
 {'timestamp':20200415,'cancel_order_ids':[],'submit_orders':[{'order_id':1,'coin':'LINKUSDT','side':0,'quantity':2.0,'reference_close':5.0}], 'global_target_notional_usd':{}},
]
base_fills=[
 {'fill_id':1,'order_id':1,'strategy_id':1,'timestamp':20200416,'coin':'LINKUSDT','side':0,'quantity':2.0,'price':6.0,'commission':0.1},
]
# Current state after the bounded fill only.
snap_a={'account_cash':87.9,'account_positions':{'LINKUSDT':2.0}}

# Same bounded economics, but the process overshot one more day with a SELL.
overfill={'fill_id':2,'order_id':2,'strategy_id':1,'timestamp':20200417,'coin':'LINKUSDT','side':1,'quantity':1.0,'price':7.0,'commission':0.2}
snap_b={'account_cash':94.7,'account_positions':{'LINKUSDT':1.0}}
overplan={'timestamp':20200416,'cancel_order_ids':[],'submit_orders':[{'order_id':2,'coin':'LINKUSDT','side':1,'quantity':1.0,'reference_close':6.5}], 'global_target_notional_usd':{}}

def fp(plans,fills,snap):
    m.get_plans=lambda root: plans
    m.get_fills=lambda root: fills
    m.snapshot=lambda root: snap
    return m.fingerprint(Path('.'),20200415,20200416)

obj_a,hash_a=fp(base_plans,base_fills,snap_a)
obj_b,hash_b=fp(base_plans+[overplan],base_fills+[overfill],snap_b)
assert hash_a==hash_b, (hash_a,hash_b,obj_a,obj_b)
assert obj_b['cash']==87.9
assert obj_b['positions']=={'LINKUSDT':2.0}
assert len(obj_b['plans'])==1 and obj_b['plans'][0]['timestamp']==20200415
assert len(obj_b['fills'])==1 and obj_b['fills'][0]['timestamp']==20200416

# A genuine economic difference inside the accepted horizon MUST still fail.
changed=dict(base_fills[0]); changed['price']=6.1
snap_changed={'account_cash':87.7,'account_positions':{'LINKUSDT':2.0}}
_,hash_changed=fp(base_plans,[changed],snap_changed)
assert hash_changed!=hash_a
print('PASS: T18 fingerprint ignores post-horizon overshoot')
print('PASS: T18 fingerprint still detects in-horizon economic differences')
PY
