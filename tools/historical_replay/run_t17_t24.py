#!/usr/bin/env python3
"""One-command acceptance campaign for T17 through T24.

Stops on first failed gate. T23 source deletion happens only after T17-T22 pass and is
backed up under deploy/historical_replay/run/ before removal.
"""
from __future__ import annotations
import argparse, datetime as dt, json, pathlib, shlex, subprocess, sys

def run(cmd,cwd,check=True):
    print("\n>>> "+" ".join(shlex.quote(str(x)) for x in cmd),flush=True)
    p=subprocess.run([str(x) for x in cmd],cwd=str(cwd),text=True,check=False)
    if check and p.returncode:
        raise SystemExit(f"ERROR: gate failed ({p.returncode}): {' '.join(map(str,cmd))}")
    return p.returncode

def load(path): return json.loads(path.read_text())

def main():
    ap=argparse.ArgumentParser(description='Run T17..T24 in one acceptance campaign')
    ap.add_argument('--root',default='.')
    ap.add_argument('--full-speed',type=float,default=10000.0,help='T19 full-history speed; immutable within that run')
    ap.add_argument('--skip-t19',action='store_true',help='diagnostic only; final T24 cannot PASS when used')
    args=ap.parse_args(); root=pathlib.Path(args.root).resolve()
    tool=root/'tools/historical_replay/run_acceptance_scenario.py'
    realtest_gate=root/'tools/historical_replay/run_t19_realtest_gate.py'
    pre=root/'validation/t17_t24_preflight.sh'
    evidence=root/'deploy/historical_replay/run'; evidence.mkdir(parents=True,exist_ok=True)

    print('============================================================')
    print('T17 -> T24 ONE-SHOT ACCEPTANCE CAMPAIGN')
    print('T18 speeds: 1500x + 1000x (short window only)')
    print('T19a capacity: 10000x, 10-day observed steady-state window (after bootstrap catch-up)')
    print(f'T19b full history: {args.full_speed:g}x, 2020-01-01 -> 2025-10-12 + open 2025-10-13')
    print('T19c RealTest: exact research policy + locked BNB/FET/ZEC accepted exceptions')
    print('T20 restart-all; T21 two-day strategy gap/catch-up; T22 NATS/PG/process chaos')
    print('T23 removes legacy shared clock; T24 replays post-cleanup and aggregates fingerprints')
    print('============================================================')

    # Structural gate + full build once for T17.
    run(['bash',str(pre),'.'],root)
    run(['ninja','-C','build','-j8'],root)
    # Compile the exact research RealTest bridge before any long-running T19 work.
    run([sys.executable,str(realtest_gate),'--root','.','--compile-only'],root)

    # T17: exact close(T) sizing -> open(T+1) fill.
    run([sys.executable,str(root/'tools/historical_replay/run_t17_execution_replay.py'),'--root','.'],root)

    # T18: clean 1500x baseline then 1000x exact fingerprint match.
    run([sys.executable,str(tool),'--root','.','--label','t18_clean_1500','--speed','1500','--target-completed','20200415','--reuse-image','--max-wall-seconds','1200'],root)
    baseline=evidence/'t18_clean_1500_summary.json'
    run([sys.executable,str(tool),'--root','.','--label','t18_clean_1000','--speed','1000','--target-completed','20200415','--reuse-image','--max-wall-seconds','1500','--expected-fingerprint',str(baseline)],root)

    # T19a: prove 10000x is sustainable before committing to the full five-year run.
    # Bootstrap is ignored until every decision stage reaches Apr 20. Then require at least
    # 10 simulated days of steady-state samples with <=2 days lag. May 10 leaves room
    # for bootstrap to catch up without weakening the capacity criterion.
    if not args.skip_t19:
        run([sys.executable,str(tool),'--root','.','--label','t19a_capacity_10000','--speed','10000','--target-completed','20200510','--reuse-image','--max-wall-seconds','900','--drain-seconds','240','--capacity-check','--capacity-observation-start','20200420','--capacity-max-lag-days','2','--capacity-min-samples','6','--capacity-min-span-days','10'],root)

        # T19b: full known dataset baseline at the proven sustainable speed.
        run([sys.executable,str(tool),'--root','.','--label','t19_full_history','--speed',format(args.full_speed,'.17g'),'--simulated-start','2020-01-01T12:00:00Z','--target-completed','20251012','--reuse-image','--max-wall-seconds','24000','--drain-seconds','900','--capacity-check','--capacity-observation-start','20200420','--capacity-max-lag-days','2','--capacity-min-samples','100','--full-baseline'],root)
        # T19c: reconstruct trades from persisted distributed fills and run the exact
        # research/src/realtest.cpp policy. Then lock the result to the historically
        # accepted BNB/FET/ZEC exception identities; arbitrary new differences fail.
        run([sys.executable,str(realtest_gate),'--root','.','--label','t19_full_history','--mode','equal-weight'],root)

    # T20: all six runtime services restart together; economy must equal clean baseline.
    run([sys.executable,str(tool),'--root','.','--label','t20_restart_all','--speed','1500','--target-completed','20200415','--fault-profile','restart-all','--reuse-image','--max-wall-seconds','1200','--expected-fingerprint',str(baseline)],root)

    # T21: deliberate multi-day Strategy downtime; acceptance is durable signal-state catch-up
    # to current business time without fatal/reconciliation errors. We do not pretend missed
    # live economic actions are retroactively executed.
    run([sys.executable,str(tool),'--root','.','--label','t21_gap_catchup','--speed','1500','--target-completed','20200415','--fault-profile','strategy-gap','--reuse-image','--max-wall-seconds','1200'],root)

    # T22: infra/process chaos; final economic fingerprint must still equal clean baseline.
    run([sys.executable,str(tool),'--root','.','--label','t22_chaos','--speed','1500','--target-completed','20200415','--fault-profile','chaos','--reuse-image','--max-wall-seconds','1500','--expected-fingerprint',str(baseline)],root)

    # T23: source cleanup only after every prior gate passed. Backup is automatic.
    run([sys.executable,str(root/'tools/historical_replay/t23_cleanup.py'),'--root','.','--apply'],root)
    run(['meson','setup','build','--reconfigure'],root)
    run(['ninja','-C','build','-j8'],root)
    run(['bash',str(root/'validation/t23_no_legacy_clock_audit.sh'),'.'],root)

    # T24: rebuild runtime image from cleaned sources and reproduce the clean fingerprint.
    run([sys.executable,str(tool),'--root','.','--label','t24_post_cleanup','--speed','1500','--target-completed','20200415','--expected-fingerprint',str(baseline),'--max-wall-seconds','1200'],root)

    names=['t18_clean_1500','t18_clean_1000','t20_restart_all','t21_gap_catchup','t22_chaos','t24_post_cleanup']
    if not args.skip_t19:
        names.insert(2,'t19a_capacity_10000')
        names.insert(3,'t19_full_history')
    reports={n:load(evidence/f'{n}_summary.json') for n in names}
    failures=[n for n,r in reports.items() if r.get('result')!='PASS']
    fp=reports['t18_clean_1500']['fingerprint_sha256']
    fp_gates=['t18_clean_1000','t20_restart_all','t22_chaos','t24_post_cleanup']
    mismatch=[n for n in fp_gates if reports[n].get('fingerprint_sha256')!=fp]
    t19_capacity_ok=(not args.skip_t19 and reports.get('t19a_capacity_10000',{}).get('capacity_ok') is True)
    t19_realtest_path=evidence/'t19_full_history_realtest_summary.json'
    t19_realtest=load(t19_realtest_path) if (not args.skip_t19 and t19_realtest_path.exists()) else {}
    t19_realtest_ok=(not args.skip_t19 and t19_realtest.get('result')=='PASS')
    t19_ok=(not args.skip_t19 and reports.get('t19_full_history',{}).get('full_baseline',{}).get('ok') is True and reports.get('t19_full_history',{}).get('capacity_ok') is True and t19_realtest_ok)
    t21_ok=reports['t21_gap_catchup'].get('gap_catchup_observed') is True
    result='PASS' if not failures and not mismatch and t19_capacity_ok and t19_ok and t21_ok else 'FAIL'
    summary={
      'result':result,'campaign':'T17-T24','generated_at':dt.datetime.now().isoformat(),
      'short_economic_fingerprint_sha256':fp,'fingerprint_gates':fp_gates,'fingerprint_mismatches':mismatch,
      't19_capacity_10000_ok':t19_capacity_ok,'t19_full_baseline_ok':(not args.skip_t19 and reports.get('t19_full_history',{}).get('full_baseline',{}).get('ok') is True),'t19_realtest_ok':t19_realtest_ok,'t19_ok':t19_ok,'t21_gap_catchup_observed':t21_ok,'failed_reports':failures,
      'reports':{n:str(evidence/f'{n}_summary.json') for n in names},
      't19_realtest_report':str(t19_realtest_path) if not args.skip_t19 else None,
      't23_cleanup':str(evidence/'t23_cleanup_summary.json')
    }
    out=evidence/'t24_final_acceptance.json'; out.write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n')
    print('\n============================================================')
    print(json.dumps(summary,indent=2,sort_keys=True))
    print('============================================================')
    if result!='PASS':
        print('FAIL: T24 aggregate acceptance did not satisfy every T17-T24 gate',file=sys.stderr); return 1
    print('PASS: T24 FINAL — T17-T24 acceptance campaign complete')
    return 0

if __name__=='__main__': raise SystemExit(main())
