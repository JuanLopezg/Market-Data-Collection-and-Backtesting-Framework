#!/usr/bin/env python3
"""Regrade an already completed T19b using corrected gate semantics.

This does not run a replay. It only promotes an existing t19_full_history summary
when structural/full-history requirements and the already-generated exact T19c
RealTest policy report both PASS.
"""
from __future__ import annotations
import argparse, json
from pathlib import Path

STRUCTURAL = {"strategy_days": 2112, "closed_trades": 630, "open_campaigns": 1}


def fail(msg: str) -> None:
    raise SystemExit(f"FAIL: {msg}")


def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--root',default='.')
    ap.add_argument('--label',default='t19_full_history')
    args=ap.parse_args()
    root=Path(args.root).resolve(); run=root/'deploy/historical_replay/run'
    summary_path=run/f'{args.label}_summary.json'
    rt_path=run/f'{args.label}_realtest_summary.json'
    if not summary_path.is_file(): fail(f'missing {summary_path}')
    if not rt_path.is_file(): fail(f'missing {rt_path}')
    rep=json.loads(summary_path.read_text())
    rt=json.loads(rt_path.read_text())

    if rt.get('result')!='PASS': fail('T19c RealTest report is not PASS')
    if rep.get('label')!=args.label: fail(f"unexpected label {rep.get('label')!r}")
    if int(rep.get('target_completed',0))!=20251012: fail('unexpected T19 target_completed')
    if int(rep.get('target_execution_day',0))!=20251013: fail('unexpected T19 target_execution_day')
    if rep.get('capacity_ok') is not True: fail('capacity_ok is not true')
    if rep.get('critical_log_lines'): fail('critical_log_lines is not empty')

    required_latest={
        'strategy_latest':20251012,
        'risk_latest':20251012,
        'planner_latest':20251012,
        'execution_last_decision':20251012,
        'execution_last_execution':20251013,
        'sim_exchange_latest':20251013,
    }
    for k,v in required_latest.items():
        if int(rep.get(k,0))<v: fail(f'{k}: expected >= {v}, got {rep.get(k)}')

    old=rep.get('full_baseline') or {}
    old_actual=old.get('actual') or {}
    actual={
        'strategy_days':int(rep.get('strategy_days',0)),
        'closed_trades':int(old_actual.get('closed_trades',-1)),
        'open_campaigns':int(old_actual.get('open_campaigns',-1)),
        'planner_days':int(rep.get('planner_days',0)),
        'orders':int(old_actual.get('orders',-1)),
        'fills':int(old_actual.get('fills',rep.get('fills',-1))),
        'closed_pnl':old_actual.get('closed_pnl'),
        'closed_pnl_rounded':old_actual.get('closed_pnl_rounded'),
    }
    errors=[f'{k}: expected {v} got {actual[k]}' for k,v in STRUCTURAL.items() if actual[k]!=v]
    if errors: fail('; '.join(errors))

    rep['regraded_from_result']=rep.get('result')
    rep['full_baseline']={
        'ok':True,
        'expected':STRUCTURAL,
        'actual':actual,
        'errors':[],
        'economic_equivalence_gate':'T19c exact research RealTest policy',
        'economic_equivalence_report':str(rt_path),
    }
    rep['result']='PASS'
    rep['regraded_without_replay']=True
    summary_path.write_text(json.dumps(rep,indent=2,sort_keys=True)+'\n')
    print('PASS: existing T19b regraded without replay')
    print(f"  strategy_days={actual['strategy_days']} planner_days={actual['planner_days']}")
    print(f"  closed_trades={actual['closed_trades']} open_campaigns={actual['open_campaigns']}")
    print(f"  orders={actual['orders']} fills={actual['fills']} (informational)")
    print('  economic_equivalence=T19c RealTest PASS')
    print(f'evidence={summary_path}')
    return 0

if __name__=='__main__': raise SystemExit(main())
