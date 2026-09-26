#!/usr/bin/env python3
"""Resume accepted T17-T21 campaign at T22 after strategy catch-up validation."""
from __future__ import annotations
import datetime as dt, json, pathlib, shlex, subprocess, sys


def run(cmd,cwd,check=True):
    print("\n>>> "+" ".join(shlex.quote(str(x)) for x in cmd),flush=True)
    p=subprocess.run([str(x) for x in cmd],cwd=str(cwd),text=True,check=False)
    if check and p.returncode:
        raise SystemExit(f"ERROR: gate failed ({p.returncode}): {' '.join(map(str,cmd))}")
    return p.returncode


def load(path): return json.loads(path.read_text())

def require_pass(path,desc):
    if not path.is_file(): raise SystemExit(f'ERROR: missing {desc}: {path}')
    obj=load(path)
    if obj.get('result')!='PASS': raise SystemExit(f"ERROR: {desc} is not PASS: {path}")
    return obj


def main():
    root=pathlib.Path('.').resolve(); evidence=root/'deploy/historical_replay/run'
    tool=root/'tools/historical_replay/run_acceptance_scenario.py'
    pre=root/'validation/t17_t24_preflight.sh'
    baseline_path=evidence/'t18_clean_1500_summary.json'

    # Fail closed: T21-only resume is legal only after every prior required gate passed.
    require_pass(baseline_path,'T18 1500x baseline')
    require_pass(evidence/'t18_clean_1000_summary.json','T18 1000x invariance')
    require_pass(evidence/'t19a_capacity_10000_summary.json','T19a capacity')
    t19=require_pass(evidence/'t19_full_history_summary.json','T19b full history')
    require_pass(evidence/'t19_full_history_realtest_summary.json','T19c RealTest')
    require_pass(evidence/'t20_restart_all_summary.json','T20 restart-all')
    if t19.get('full_baseline',{}).get('ok') is not True:
        raise SystemExit('ERROR: T19b corrected structural baseline is not PASS')
    if t19.get('capacity_ok') is not True:
        raise SystemExit('ERROR: T19b capacity gate is not PASS')

    t21=require_pass(evidence/'t21_gap_catchup_summary.json','T21 strategy gap catch-up')
    if t21.get('gap_catchup_observed') is not True:
        raise SystemExit('ERROR: T21 evidence is PASS but gap_catchup_observed is not true')

    print('PASS: existing T18-T21 evidence accepted; resuming at T22')
    run(['bash',str(pre),'.'],root)
    run(['ninja','-C','build','-j8'],root)

    # Rebuild the runtime image once so T22 uses the same strategy binary that passed T21.
    run([str(root/'deploy/historical_replay/build_runtime_bundle.sh')],root)
    te=root/'deploy/historical_replay/run/time.env'; te.parent.mkdir(parents=True,exist_ok=True); te.write_text('# build placeholder\n')
    run(['docker','compose','--env-file',str(root/'deploy/historical_replay/.env'),'-f',str(root/'deploy/historical_replay/docker-compose.yml'),'--profile','runtime','build'],root)

    run([sys.executable,str(tool),'--root','.','--label','t22_chaos','--speed','1500','--target-completed','20200415','--fault-profile','chaos','--reuse-image','--max-wall-seconds','1500','--expected-fingerprint',str(baseline_path)],root)

    # Destructive T23 stays gated behind successful T21 and T22.
    run([sys.executable,str(root/'tools/historical_replay/t23_cleanup.py'),'--root','.','--apply'],root)
    run(['meson','setup','build','--reconfigure'],root)
    run(['ninja','-C','build','-j8'],root)
    run(['bash',str(root/'validation/t23_no_legacy_clock_audit.sh'),'.'],root)

    run([sys.executable,str(tool),'--root','.','--label','t24_post_cleanup','--speed','1500','--target-completed','20200415','--expected-fingerprint',str(baseline_path),'--max-wall-seconds','1200'],root)

    names=['t18_clean_1500','t18_clean_1000','t19a_capacity_10000','t19_full_history','t20_restart_all','t21_gap_catchup','t22_chaos','t24_post_cleanup']
    reports={n:load(evidence/f'{n}_summary.json') for n in names}
    failures=[n for n,r in reports.items() if r.get('result')!='PASS']
    fp=reports['t18_clean_1500']['fingerprint_sha256']
    fp_gates=['t18_clean_1000','t20_restart_all','t22_chaos','t24_post_cleanup']
    mismatch=[n for n in fp_gates if reports[n].get('fingerprint_sha256')!=fp]
    t19_realtest=load(evidence/'t19_full_history_realtest_summary.json')
    t19_ok=(reports['t19_full_history'].get('full_baseline',{}).get('ok') is True and reports['t19_full_history'].get('capacity_ok') is True and t19_realtest.get('result')=='PASS')
    t21_ok=reports['t21_gap_catchup'].get('gap_catchup_observed') is True
    result='PASS' if not failures and not mismatch and t19_ok and t21_ok else 'FAIL'
    summary={
      'result':result,'campaign':'T17-T24','resumed_at':'T22','generated_at':dt.datetime.now().isoformat(),
      'short_economic_fingerprint_sha256':fp,'fingerprint_gates':fp_gates,'fingerprint_mismatches':mismatch,
      't19_capacity_10000_ok':reports['t19a_capacity_10000'].get('capacity_ok') is True,
      't19_full_baseline_ok':reports['t19_full_history'].get('full_baseline',{}).get('ok') is True,
      't19_realtest_ok':t19_realtest.get('result')=='PASS','t19_ok':t19_ok,
      't21_gap_catchup_observed':t21_ok,'failed_reports':failures,
      'reports':{n:str(evidence/f'{n}_summary.json') for n in names},
      't19_realtest_report':str(evidence/'t19_full_history_realtest_summary.json'),
      't23_cleanup':str(evidence/'t23_cleanup_summary.json')
    }
    out=evidence/'t24_final_acceptance.json'; out.write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n')
    print('\n============================================================')
    print(json.dumps(summary,indent=2,sort_keys=True))
    print('============================================================')
    if result!='PASS':
        print('FAIL: T24 aggregate acceptance did not satisfy every T17-T24 gate',file=sys.stderr); return 1
    print('PASS: T24 FINAL — T17-T24 acceptance campaign complete (resumed from accepted T21 evidence)')
    return 0

if __name__=='__main__': raise SystemExit(main())
