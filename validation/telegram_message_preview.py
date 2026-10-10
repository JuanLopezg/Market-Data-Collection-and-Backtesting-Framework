#!/usr/bin/env python3
"""Explicit opt-in Telegram format previews; examples are visibly labelled TEST."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--send', action='store_true', help='Send three labelled examples using local Telegram settings')
    args = parser.parse_args()
    identity = 'telegram-preview-' + str(time.time_ns())
    directory = ROOT / 'storage' / 'telegram_previews' / identity
    source, receipts = directory / 'source', directory / 'notifier'
    source.mkdir(parents=True)
    receipts.mkdir()
    clock = datetime.now(timezone.utc).isoformat()
    examples = (
        ('WARN', 'USD_RESERVE', 'TEST: USD reserve low', 'Available: $2.18\nRequired: $183.76'),
        ('CRITICAL', 'MAINTENANCE_MARGIN_BREACH', 'TEST: Maintenance margin breach', 'Margin headroom: -$25.00'),
        ('INFO', 'DAILY_PORTFOLIO', 'TEST: Daily portfolio',
         'Example values\nBalance: $1,000.00\nEquity: $1,025.00\nActive positions (USD exposure):\n'
         'BTC LONG: $500.00\nETH SHORT: $250.00\nYesterday\'s positions:\nBTC LONG: $450.00'),
    )
    events = []
    for index, (severity, kind, title, detail) in enumerate(examples):
        event_id = identity + ':' + str(index)
        events.append({'eventId': event_id, 'recordedAt': clock, 'transition': 'OPENED', 'alert': {
            'id': event_id, 'timestamp': clock, 'severity': severity, 'status': 'ACTIVE',
            'account': 'Kraken (TEST)', 'service': 'TelegramPreview', 'eventType': kind,
            'title': title, 'detail': detail}})
    (source / 'events.jsonl').write_text(''.join(json.dumps(event) + '\n' for event in events))
    (source / 'status.json').write_text(json.dumps({'version': 'step43-v1', 'lastSweepAt': clock,
                                                  'lastSuccessAt': clock}))
    if not args.send:
        print('Preview source:', source, '| no messages sent; use --send for explicit delivery')
        return
    def docker(*arguments):
        return subprocess.run(['docker', *arguments], capture_output=True, text=True, check=True, timeout=30).stdout
    context = json.loads(docker('context', 'inspect'))[0]
    if not context['Endpoints']['docker']['Host'].startswith(('unix://', 'npipe://')):
        raise SystemExit('Preview requires a local Docker endpoint')
    try:
        docker('run', '-d', '--name', identity,
            '--env-file', str(ROOT / 'storage/paper_trading/.env'),
            '-e', 'DASHBOARD_NOTIFIER_SINK=TELEGRAM', '-e', 'DASHBOARD_NOTIFIER_MIN_SEVERITY=INFO',
            '-e', 'DASHBOARD_ALERT_STORE_DIR=/source', '-e', 'DASHBOARD_NOTIFIER_STORE_DIR=/data/notifier',
            '-e', 'DASHBOARD_NOTIFIER_INTERVAL=1s',
            '-e', 'DASHBOARD_TELEGRAM_BOT_TOKEN_FILE=', '-e', 'DASHBOARD_TELEGRAM_CHAT_ID_FILE=',
            '-e', 'DASHBOARD_TELEGRAM_RECEIPT_FILE=/data/notifier/telegram-receipts.jsonl',
            '-v', str(source) + ':/source:ro', '-v', str(receipts) + ':/data/notifier',
            '--entrypoint', '/dashboard-alert-notifier', 'algotrading-telegram-api')
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            status_file = receipts / 'status.json'
            if status_file.exists():
                status = json.loads(status_file.read_text())
                if status.get('deliveredCount') == 3:
                    print('Telegram accepted three TEST examples. Receipts:', receipts)
                    return
            time.sleep(1)
        raise SystemExit('Preview delivery not fully confirmed; check sanitized notifier status')
    finally:
        subprocess.run(['docker', 'rm', '-f', identity], capture_output=True, timeout=30)


if __name__ == '__main__':
    main()
