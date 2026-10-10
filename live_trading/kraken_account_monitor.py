#!/usr/bin/env python3
"""Read-only Kraken account polling and durable events for the existing notifier."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time

from kraken_account import AccountJournal, PRIVATE_SCOPE, ROOT, reader_from_credentials
from kraken_portfolio_summary import DailyPortfolio, dollars, position_values
from kraken_shadow import KrakenPublicReader
from shadow_observer import number


def utc(timestamp):
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def atomic_text(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(value, encoding='utf-8')
    temporary.replace(path)


def warnings(result):
    """Group current/stressed reserve warnings; keep critical margin alerts distinct."""
    reasons = set(result['reasons'])
    alerts = {}
    reserve_reasons = reasons & {'USD_RESERVE_LOW', 'USD_RESERVE_LOW_AFTER_LOSS_STRESS',
                                'USD_BALANCE_NOT_REPORTED'}
    if reserve_reasons:
        reserve = result['usd_reserve']
        detail = ('USD balance unavailable' if reserve is None else
                  'Available: ' + dollars(reserve['usd_available_now']) +
                  '\nRequired: ' + dollars(reserve['minimum_usd_reserve']))
        if reserve and reserve['usd_after_assumed_debits_and_unrealized_loss'] != reserve['usd_available_now']:
            detail += '\nAfter loss/charge stress: ' + dollars(reserve['usd_after_assumed_debits_and_unrealized_loss'])
        alerts['USD_RESERVE'] = ('WARN', 'USD reserve low', detail)
    for reason, severity, title, detail in (
        ('ACCOUNT_EQUITY_BELOW_MINIMUM', 'WARN', 'Account equity below minimum',
         'Equity: ' + dollars(result['venue_totals_usd']['marginEquity'])),
        ('INITIAL_MARGIN_SHORTFALL', 'CRITICAL', 'Initial margin shortfall',
         'Headroom including orders: ' + dollars(result['initial_margin_headroom_with_orders_usd'])),
        ('MAINTENANCE_MARGIN_BREACH', 'CRITICAL', 'Maintenance margin breach',
         'Margin headroom: ' + dollars(result['maintenance_margin_headroom_usd'])),
    ):
        if reason in reasons:
            alerts[reason] = (severity, title, detail)
    return alerts


def account_warnings(result, market, now, yesterday, decline_fraction, gross_limit):
    """Observe user-selected thresholds only; no stop, order or account mutation."""
    alerts = {}
    unavailable = set()
    equity = number(result['venue_totals_usd']['marginEquity'])
    previous = number(yesterday['equity']) if yesterday else None
    if previous is None or previous <= 0:
        unavailable.add('DAILY_EQUITY_DROP')
    elif (previous - equity) / previous >= decline_fraction:
        drop = (previous - equity) / previous
        alerts['DAILY_EQUITY_DROP'] = ('WARN', 'Equity down versus yesterday',
            'Drop: ' + format(drop * 100, '.2f') + '%\nYesterday: ' + dollars(previous) +
            '\nNow: ' + dollars(equity))
    positions = position_values(result, market, now)
    if equity <= 0 or any(row['notional_usd'] is None for row in positions):
        unavailable.add('GROSS_EXPOSURE')
    else:
        gross = sum((number(row['notional_usd']) for row in positions), number(0))
        leverage = gross / equity
        if leverage > gross_limit:
            alerts['GROSS_EXPOSURE'] = ('WARN', 'Gross exposure above ' + str(gross_limit) + 'x',
                'Exposure: ' + dollars(gross) + '\nEquity: ' + dollars(equity) +
                '\nRatio: ' + format(leverage, '.2f') + 'x')
    return alerts, unavailable


def start_notifier(directory):
    """Reuse the local Go notifier; Docker consumes Telegram credentials internally."""
    def docker(*args):
        return subprocess.run(['docker', *args], check=True, capture_output=True, text=True, timeout=30).stdout.strip()
    context = json.loads(docker('context', 'inspect'))[0]
    host = context['Endpoints']['docker']['Host']
    if not host.startswith(('unix://', 'npipe://')):
        raise ValueError('Use a local Docker endpoint only')
    identity = hashlib.sha256(str(directory).encode()).hexdigest()[:12]
    name = 'algotrading-kraken-alerts-' + identity
    existing = docker('ps', '-a', '--filter', 'name=^/' + name + '$', '--format', '{{.Names}}')
    if existing:
        label = docker('inspect', '--format', '{{index .Config.Labels "algotrading.kraken-alert-source"}}', name)
        if label != str(directory):
            raise ValueError('Notifier container belongs to another source')
        current_image = docker('image', 'inspect', '--format', '{{.Id}}', 'algotrading-telegram-api')
        installed_image = docker('inspect', '--format', '{{.Image}}', name)
        if current_image != installed_image:
            # Replace only this owned local container; retain its named receipt volume.
            docker('stop', name)
            docker('rm', name)
            existing = ''
        else:
            docker('start', name)
    if not existing:
        docker('run', '-d', '--name', name, '--restart', 'unless-stopped',
            '--label', 'algotrading.kraken-alert-source=' + str(directory),
            '--env-file', str(ROOT / 'storage/paper_trading/.env'),
            '-e', 'DASHBOARD_NOTIFIER_SINK=TELEGRAM',
            '-e', 'DASHBOARD_ALERT_STORE_DIR=/source',
            '-e', 'DASHBOARD_NOTIFIER_STORE_DIR=/data/notifier',
            '-e', 'DASHBOARD_NOTIFIER_MIN_SEVERITY=INFO',
            '-e', 'DASHBOARD_NOTIFIER_INTERVAL=5s',
            '-e', 'DASHBOARD_TELEGRAM_BOT_TOKEN_FILE=', '-e', 'DASHBOARD_TELEGRAM_CHAT_ID_FILE=',
            '-e', 'DASHBOARD_TELEGRAM_RECEIPT_FILE=/data/notifier/telegram-receipts.jsonl',
            '-v', str(directory) + ':/source:ro',
            '-v', name + ':/data/notifier', '--entrypoint', '/dashboard-alert-notifier',
            'algotrading-telegram-api')
    atomic_text(directory / 'notifier.json', json.dumps({'container': name, 'scope': 'LOCAL_ONLY'}) + '\n')


class AccountAlertMonitor:
    """Own one account/policy journal and persist transitions across process restarts."""

    def __init__(self, directory, policy, environment='live', scope=PRIVATE_SCOPE,
                 account_name='Kraken', summary_hour_utc=None, decline_fraction='0.10', gross_limit='5'):
        self.decline_fraction, self.gross_limit = number(decline_fraction), number(gross_limit)
        if not 0 < self.decline_fraction <= 1 or self.gross_limit <= 0:
            raise ValueError('Invalid account warning thresholds')
        self.unavailable_checks = []
        self.directory = Path(directory)
        self.journal = AccountJournal(self.directory, scope, environment, policy)
        self.db = self.journal.db
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS alert_state (id TEXT PRIMARY KEY, active INTEGER NOT NULL,
                sequence INTEGER NOT NULL, severity TEXT NOT NULL, title TEXT NOT NULL, detail TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS alert_events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS alert_clock (id INTEGER PRIMARY KEY CHECK(id=1), observed_at REAL NOT NULL);
        ''')
        self.account_name = account_name
        self.summary = DailyPortfolio(self.db, account_name, summary_hour_utc)

    def close(self):
        self.journal.close()

    def observe(self, bundle, now, market=None):
        result = self.journal.observe(bundle, now)
        desired = warnings(result)
        extra, unavailable = account_warnings(result, market, now, self.summary.yesterday(result['observed_at']),
                                              self.decline_fraction, self.gross_limit)
        desired.update(extra)
        timestamp = result['observed_at']
        with self.db:
            previous = self.db.execute('SELECT observed_at FROM alert_clock WHERE id=1').fetchone()
            if previous and timestamp < previous[0]:
                raise ValueError('Out-of-order alert observation')
            states = {row[0]: row[1:] for row in self.db.execute('SELECT * FROM alert_state')}
            for name in sorted(set(states) | set(desired)):
                if name in unavailable:
                    continue  # Unknown evidence must not manufacture a resolution.
                old = states.get(name, (0, 0, '', '', ''))
                active = name in desired
                if bool(old[0]) == active:
                    continue
                severity, title, detail = desired[name] if active else old[2:]
                sequence = old[1] + 1
                alert_id = 'kraken-account:' + result['account_identity_hash'] + ':' + name
                transition = 'OPENED' if active else 'RESOLVED'
                event = {'eventId': alert_id + ':' + str(sequence), 'recordedAt': utc(timestamp),
                    'transition': transition, 'alert': {'id': alert_id, 'timestamp': utc(timestamp),
                    'severity': severity, 'status': 'ACTIVE' if active else 'RESOLVED',
                    'service': 'KrakenAccountReadOnly', 'account': self.account_name, 'eventType': name,
                    'title': title if active else title + ' resolved',
                    'detail': detail if active else 'This warning has cleared.'}}
                self.db.execute('INSERT INTO alert_events(payload) VALUES (?)', (json.dumps(event),))
                self.db.execute('INSERT OR REPLACE INTO alert_state VALUES (?,?,?,?,?,?)',
                                (name, int(active), sequence, severity, title, detail))
            self.db.execute('INSERT OR REPLACE INTO alert_clock VALUES (1,?)', (timestamp,))
            if self.summary:
                summary = self.summary.record(result, market, now)
                if summary:
                    self.db.execute('INSERT INTO alert_events(payload) VALUES (?)', (json.dumps(summary),))
        self.unavailable_checks = sorted(unavailable)
        self.publish(now)
        return result

    def publish(self, now, error=''):
        # Publish history before heartbeat. A failed/stale read never emits recovery.
        history = ''.join(row[0] + '\n' for row in self.db.execute('SELECT payload FROM alert_events ORDER BY sequence'))
        atomic_text(self.directory / 'events.jsonl', history)
        last = self.db.execute('SELECT observed_at FROM alert_clock WHERE id=1').fetchone()
        active = list(self.db.execute('SELECT severity FROM alert_state WHERE active=1'))
        heartbeat = {'version': 'step43-v1', 'lastSweepAt': utc(now),
            'lastSuccessAt': utc(last[0]) if last else '', 'lastError': error,
            'unavailableChecks': self.unavailable_checks,
            'activeCount': len(active), 'activeCritical': sum(row[0] == 'CRITICAL' for row in active),
            'activeWarnings': sum(row[0] == 'WARN' for row in active),
            'eventCount': self.db.execute('SELECT count(*) FROM alert_events').fetchone()[0]}
        atomic_text(self.directory / 'status.json', json.dumps(heartbeat, indent=2) + '\n')

    def poll(self, reader, now=time.time, public_reader=None):
        try:
            bundle = reader.snapshot()
            market = None
            if self.summary and public_reader and bundle['positions']['openPositions']:
                try:
                    market = public_reader.snapshot()
                except Exception:
                    pass  # Missing price evidence is shown explicitly in the summary.
            return self.observe(bundle, now(), market)
        except Exception:
            # No upstream body, URL, credential, exception text or traceback escapes.
            self.publish(now(), 'Account observation unavailable or invalid; recovery not inferred')
            return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--telegram', action='store_true', help='Start/reuse the existing local Go Telegram notifier')
    parser.add_argument('--environment', choices=('live', 'demo'), default='live')
    parser.add_argument('--interval', type=float, default=30)
    parser.add_argument('--account-name', default='Kraken')
    parser.add_argument('--summary-hour-utc', type=int, default=9)
    parser.add_argument('--daily-equity-drop-fraction', default='0.10')
    parser.add_argument('--gross-exposure-warning', default='5')
    parser.add_argument('--policy', type=Path, default=ROOT / 'storage/kraken_shadow/account-policy.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'storage/kraken_shadow/account-alerts')
    args = parser.parse_args()
    if not 0 <= args.summary_hour_utc <= 23 or not args.account_name.strip() or len(args.account_name) > 64 or any(ord(c) < 32 for c in args.account_name):
        parser.error('Use a single-line account name and a summary UTC hour from 0 to 23')
    directory = args.output.resolve()
    if not directory.is_relative_to((ROOT / 'storage/kraken_shadow').resolve()) or not 10 <= args.interval <= 60:
        parser.error('Use ignored Kraken storage and a polling interval between 10 and 60 seconds')
    directory.mkdir(parents=True, exist_ok=True)
    lock = (directory / 'monitor.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit('KRAKEN-MONITOR: another process owns this output directory') from None
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    signal.signal(signal.SIGINT, lambda *_: stopped.set())
    monitor = None
    try:
        policy = json.loads(args.policy.read_text(encoding='utf-8'))
        reader = reader_from_credentials(ROOT / 'storage/kraken_shadow/.env', args.environment)
        monitor = AccountAlertMonitor(directory, policy, args.environment,
            account_name=args.account_name.strip(), summary_hour_utc=args.summary_hour_utc,
            decline_fraction=args.daily_equity_drop_fraction, gross_limit=args.gross_exposure_warning)
        public_reader = KrakenPublicReader(args.environment, directory / 'public-prices')
        atomic_text(directory / 'monitor.pid', str(os.getpid()) + '\n')
        notifier_started = False
        while not stopped.is_set():
            result = monitor.poll(reader, public_reader=public_reader)
            if result is not None and args.telegram and not notifier_started:
                start_notifier(directory)
                notifier_started = True
            if args.once:
                print('KRAKEN-MONITOR:', 'read-only observation completed' if result else 'observation failed safely')
                return 0 if result else 1
            stopped.wait(args.interval)
    except Exception:
        # Invalidate any prior fresh source on startup or notifier/persistence failure.
        atomic_text(directory / 'status.json', json.dumps({'version': 'step43-v1',
            'lastSuccessAt': '', 'lastSweepAt': utc(time.time()),
            'lastError': 'Monitor initialization or persistence failed'}))
        raise SystemExit('KRAKEN-MONITOR: initialization or persistence failed; check local policy and credential setup') from None
    finally:
        if monitor:
            monitor.close()
        (directory / 'monitor.pid').unlink(missing_ok=True)
        lock.close()


if __name__ == '__main__':
    raise SystemExit(main())
