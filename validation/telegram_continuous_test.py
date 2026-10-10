#!/usr/bin/env python3
"""Explicit local real-Telegram acceptance for continuous delivery and recovery."""
import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import secrets
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--send-real', action='store_true', help='authorize two clearly marked Telegram test messages')
    parser.add_argument('--env-file', type=Path, default=ROOT / 'storage/paper_trading/.env')
    parser.add_argument('--image', default='algotrading-telegram-api')
    args = parser.parse_args()
    if not args.send_real:
        parser.error('--send-real is required; this test sends actual Telegram messages')
    context = os.environ.get('DOCKER_CONTEXT')
    endpoint = None if context else os.environ.get('DOCKER_HOST')
    if not endpoint:
        endpoint = json.loads(subprocess.check_output(['docker', 'context', 'inspect', *([context] if context else [])], text=True))[0]['Endpoints']['docker']['Host']
    if not endpoint.startswith(('unix://', 'npipe://')):
        raise ValueError('Only local Docker endpoints are allowed')
    values = {}
    for line in args.env_file.read_text(encoding='utf-8-sig').splitlines():
        if '=' in line and not line.strip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    token, chat = values.get('DASHBOARD_TELEGRAM_BOT_TOKEN', ''), values.get('DASHBOARD_TELEGRAM_CHAT_ID', '')
    if not token or ':' not in token or not chat:
        raise ValueError('Configured Telegram token and chat ID are required')
    project = 'algotrading-telegram-' + secrets.token_hex(6)
    directory = ROOT / 'storage/telegram_validation' / project
    source = directory / 'source'
    source.mkdir(parents=True)
    state_volume = project + '-state'
    envfile = directory / '.env'
    envfile.write_text('DASHBOARD_NOTIFIER_SINK=TELEGRAM\n'
                       f'DASHBOARD_TELEGRAM_BOT_TOKEN={token}\nDASHBOARD_TELEGRAM_CHAT_ID={chat}\n'
                       'DASHBOARD_ALERT_STORE_DIR=/fixture\nDASHBOARD_NOTIFIER_STORE_DIR=/data/notifier\n'
                       'DASHBOARD_NOTIFIER_INTERVAL=1s\nDASHBOARD_NOTIFIER_SWEEP_TIMEOUT=10s\n')
    envfile.chmod(0o600)

    def run(*command):
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=60)
        output = (result.stdout + result.stderr).replace(token, '[REDACTED_TOKEN]').replace(chat, '[REDACTED_CHAT]')
        with (directory / 'operations.log').open('a') as log:
            log.write(output + '\n')
        if result.returncode:
            raise RuntimeError('Local test command failed; sanitized evidence: ' + str(directory))
        return result.stdout.strip()

    def heartbeat(stale=False):
        now = datetime.now(timezone.utc) - (timedelta(seconds=91) if stale else timedelta())
        temporary = source / 'status.tmp'
        temporary.write_text(json.dumps({'version': 'step43-v1', 'lastSuccessAt': now.isoformat(), 'lastSweepAt': now.isoformat()}))
        temporary.replace(source / 'status.json')

    def status():
        return json.loads(run('docker', 'exec', project, 'cat', '/data/notifier/status.json'))

    def wait(predicate, refresh=True):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if refresh:
                heartbeat()
            observed = status()
            if predicate(observed):
                return observed
            time.sleep(1)
        raise AssertionError('Notifier did not reach the expected state; evidence: ' + str(directory))

    def event(identity, transition):
        now = datetime.now(timezone.utc).isoformat()
        return {'eventId': project + ':' + identity, 'recordedAt': now, 'transition': transition,
                'alert': {'id': project + ':alert', 'timestamp': now, 'severity': 'WARN',
                          'status': 'RESOLVED' if transition == 'RESOLVED' else 'ACTIVE',
                          'service': 'LocalTelegramTest', 'eventType': 'CONTROLLED_TEST',
                          'title': 'TEST ONLY: Telegram continuous alert check',
                          'detail': 'Controlled notifier test. No trading incident or order was created.'}}

    def append(record):
        with (source / 'events.jsonl').open('a') as journal:
            journal.write(json.dumps(record) + '\n')
        heartbeat()

    (source / 'events.jsonl').write_text('')
    heartbeat()
    started = False
    try:
        run('docker', 'run', '-d', '--name', project, '--network', 'bridge', '--env-file', str(envfile),
            '-v', str(source) + ':/fixture:ro', '-v', state_volume + ':/data/notifier',
            '--entrypoint', '/dashboard-alert-notifier', args.image)
        started = True
        # The packaged process creates its first heartbeat before subsequent checks.
        time.sleep(2)
        wait(lambda s: s.get('bootstrapComplete') and not s.get('lastError'))
        run('docker', 'network', 'disconnect', 'bridge', project)
        append(event('open', 'OPENED'))
        failed = wait(lambda s: bool(s.get('lastError')) and s['sourceEventCount'] == 1)
        assert failed['deliveredCount'] == 0 and failed['processedCount'] == 0
        run('docker', 'network', 'connect', 'bridge', project)
        delivered = wait(lambda s: s['deliveredCount'] == 1 and not s.get('lastError'))
        run('docker', 'restart', project)
        wait(lambda s: s['deliveredCount'] == 1 and not s.get('lastError') and s['lastSweepAt'] != delivered['lastSweepAt'])
        heartbeat(stale=True)
        wait(lambda s: 'stale' in s.get('lastError', ''), refresh=False)
        heartbeat()
        wait(lambda s: s['deliveredCount'] == 1 and not s.get('lastError'))
        append(event('resolved', 'RESOLVED'))
        resolved = wait(lambda s: s['deliveredCount'] == 2 and not s.get('lastError'))
        run('docker', 'restart', project)
        final = wait(lambda s: s['deliveredCount'] == 2 and not s.get('lastError') and s['lastSweepAt'] != resolved['lastSweepAt'])
        receipts = run('docker', 'exec', project, 'cat', '/data/notifier/telegram-receipts.jsonl')
        assert len(receipts.splitlines()) == 2
        (directory / 'receipts.jsonl').write_text(receipts + '\n')
        (directory / 'accepted.json').write_text(json.dumps({'real_messages_accepted': 2,
            'network_failure_left_event_unprocessed': True, 'network_recovery_delivered': True,
            'stale_source_blocked': True, 'source_recovery_passed': True,
            'restart_no_duplicate_receipts': True, 'final_status': final}, indent=2) + '\n')
        print('TELEGRAM CONTINUOUS: PASS: real OPENED/RESOLVED, network and source recovery, restart without duplicate receipts')
        print('Evidence:', directory)
    finally:
        try:
            if started:
                run('docker', 'logs', project)
        finally:
            subprocess.run(['docker', 'rm', '-f', project], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['docker', 'volume', 'rm', state_volume], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
