#!/usr/bin/env python3
from __future__ import annotations
import argparse, datetime as dt, json, pathlib, shutil, sys

LEGACY_FILES = [
    'lib/src/contracts/clock_state.h',
    'lib/src/contracts/clock_control.h',
    'lib/src/contracts/clock_sync_request.h',
    'lib/src/runtime/clock.h',
    'lib/src/runtime/service_clock.h',
    'lib/src/runtime/runtime_mode.h',
]
LEGACY_DIRS = [
    'live_trading/replay_controller',
    'deploy/distributed_replay',
    'tools/distributed_compare',
]
PAYLOAD_FILES = [
    'lib/src/transport/message_json.h',
    'lib/src/transport/message_json.cpp',
    'lib/src/transport/message_subjects.h',
    'live_trading/meson.build',
]
FORBIDDEN = [
    'ClockState', 'ClockControl', 'ClockSyncRequest', 'ServiceClockContext',
    'SimulatedClock', 'CLOCK_STATE', 'CLOCK_CONTROL', 'CLOCK_SYNC_REQUEST',
    'simulation.clock.state', 'simulation.clock.control', 'simulation.clock.sync',
    'service_clock.h', 'runtime_mode.h',
]

def copy_any(src: pathlib.Path, dst: pathlib.Path) -> None:
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

def main() -> int:
    ap=argparse.ArgumentParser(description='T23 remove legacy shared-clock implementation')
    ap.add_argument('--root', default='.')
    ap.add_argument('--apply', action='store_true')
    args=ap.parse_args()
    root=pathlib.Path(args.root).resolve()
    payload=root/'tools/historical_replay/t23_payload'
    missing=[x for x in PAYLOAD_FILES if not (payload/x).exists()]
    if missing:
        raise SystemExit('missing T23 payload: '+', '.join(missing))
    plan={'replace':PAYLOAD_FILES,'delete_files':LEGACY_FILES,'delete_dirs':LEGACY_DIRS}
    if not args.apply:
        print(json.dumps(plan, indent=2))
        return 0

    stamp=dt.datetime.now().strftime('%Y%m%d_%H%M%S')
    backup=root/'deploy/historical_replay/run'/f't23_legacy_clock_backup_{stamp}'
    backup.mkdir(parents=True, exist_ok=True)
    for rel in PAYLOAD_FILES + LEGACY_FILES + LEGACY_DIRS:
        src=root/rel
        if src.exists():
            copy_any(src, backup/rel)

    for rel in PAYLOAD_FILES:
        copy_any(payload/rel, root/rel)
    for rel in LEGACY_FILES:
        p=root/rel
        if p.exists(): p.unlink()
    for rel in LEGACY_DIRS:
        p=root/rel
        if p.exists(): shutil.rmtree(p)

    roots=[root/'lib', root/'live_trading']
    hits=[]
    for base in roots:
        for p in base.rglob('*'):
            if not p.is_file() or p.suffix not in {'.h','.hpp','.c','.cc','.cpp','.cxx','.build'}:
                continue
            try: text=p.read_text(encoding='utf-8')
            except UnicodeDecodeError: continue
            for token in FORBIDDEN:
                if token in text:
                    hits.append(f'{p.relative_to(root)}: {token}')
                    break
    if hits:
        print('T23 cleanup left forbidden legacy clock references:', file=sys.stderr)
        for h in hits: print('  '+h, file=sys.stderr)
        print(f'backup={backup}', file=sys.stderr)
        return 1

    report={'result':'PASS','backup':str(backup),'deleted_files':LEGACY_FILES,'deleted_dirs':LEGACY_DIRS,'replaced':PAYLOAD_FILES}
    out=root/'deploy/historical_replay/run'/'t23_cleanup_summary.json'
    out.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(json.dumps(report,indent=2,sort_keys=True))
    print('PASS: T23 legacy shared-clock implementation removed')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
