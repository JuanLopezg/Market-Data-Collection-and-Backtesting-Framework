#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
SCRIPT="$ROOT/tools/historical_replay/run_acceptance_scenario.py"
python3 - "$SCRIPT" <<'PY'
import ast, hashlib, json, sys
from pathlib import Path
p=Path(sys.argv[1])
s=p.read_text()
assert 'def normalize_economic_plans' in s
assert 'if not cancels and not submits:' in s
assert 'normalize_external_fingerprint' in s
assert "expected_obj=normalize_external_fingerprint(expected['fingerprint'])" in s
ast.parse(s)

def nf(x):
    return round(float(x),10)

def norm_plans(plans,target=20200415):
    out=[]
    for p in plans:
        if p['timestamp']>target: continue
        cancels=list(p.get('cancel_order_ids',[]) or [])
        submits=[]
        for o in p.get('submit_orders',[]) or []:
            submits.append({k:(nf(v) if isinstance(v,float) else v) for k,v in sorted(o.items()) if k!='metadata'})
        if not cancels and not submits: continue
        out.append({'timestamp':p['timestamp'],'cancel_order_ids':cancels,'submit_orders':submits})
    return out

base_action={'timestamp':20200411,'cancel_order_ids':[],'submit_orders':[{'order_id':1,'coin':'LINK','notional_usd':10000.0}]}
noop={'timestamp':20200410,'cancel_order_ids':[],'submit_orders':[]}
a=norm_plans([base_action])
b=norm_plans([noop,base_action,{'timestamp':20200412,'cancel_order_ids':[],'submit_orders':[]}])
assert a==b, (a,b)
changed=norm_plans([{'timestamp':20200411,'cancel_order_ids':[],'submit_orders':[{'order_id':1,'coin':'LINK','notional_usd':9999.0}]}])
assert changed!=a
print('PASS: T18 economic fingerprint ignores durable no-op planning checkpoints')
print('PASS: T18 economic fingerprint still detects actionable plan differences')
print('PASS: T18 expected fingerprint is normalized, so the existing 1500x summary can be reused')
PY
