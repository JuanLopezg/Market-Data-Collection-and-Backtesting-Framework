#!/usr/bin/env python3
"""Read-only Kraken account polling and durable events for the existing notifier."""
import argparse
import logging
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import threading
import time

from kraken_account import (AccountJournal, AccountReadError, CredentialError, JournalError, PRIVATE_SCOPE,
                            ROOT, account_log_summary, reader_from_credentials)
from kraken_portfolio_summary import DailyPortfolio, collateral_risk, dollars, percent, position_values
from kraken_shadow import KrakenPublicReader
from shadow_observer import digest, number
from readable_log import setup_logging

# Opened by the monitor itself when Kraken cannot be read for a while (the other alerts
# come from account observations). Resolved by the next good read.
UNREADABLE = 'KRAKEN_UNREADABLE'
ACCOUNT_LOG_INTERVAL_SECONDS = 3600
ACCOUNT_LOG_RETRY_SECONDS = 300


def utc(timestamp):
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def utc_minute(timestamp):
    return datetime.fromtimestamp(timestamp, timezone.utc).strftime('%Y-%m-%d %H:%M UTC')


def failure_cause(error):
    """Our own bounded wording only: the reader's diagnostics never carry bodies or keys."""
    if isinstance(error, (AccountReadError, CredentialError)):
        return str(error)
    return 'Kraken invalid account data'


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


def account_warnings(result, market, now, yesterday, decline_fraction, gross_limit, maintenance_drop):
    """Observe user-selected thresholds only; no stop, order or account mutation."""
    alerts = {}
    unavailable = set()
    # User request 2026-10-14: warn when a fall of BTC and the net long positions smaller
    # than this fraction would reach the maintenance margin.
    risk = collateral_risk(result, market, now)
    drop = risk['crypto_drop_to_maintenance']
    if drop is None:
        unavailable.add('MAINTENANCE_DROP')
    elif drop != 'NONE' and number(drop) < maintenance_drop:
        alerts['MAINTENANCE_DROP'] = ('WARN', 'Close to maintenance margin',
            'A fall of ' + percent(drop) + ' in BTC and the long positions would reach it' +
            '\nBTC alone: ' + percent(risk['btc_drop_to_maintenance']) +
            '\nHeadroom: ' + dollars(risk['maintenance_headroom_usd']) +
            '\nWarning below: ' + format(maintenance_drop * 100, '.0f') + '%')
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
                 account_name='Kraken', summary_hour_utc=None, decline_fraction='0.10', gross_limit='5',
                 retention_days=None, maintenance_drop='0.25', unreadable_minutes=10):
        self.decline_fraction, self.gross_limit = number(decline_fraction), number(gross_limit)
        self.maintenance_drop = number(maintenance_drop)
        if not 0 < self.decline_fraction <= 1 or self.gross_limit <= 0 or not 0 < self.maintenance_drop <= 1:
            raise ValueError('Invalid account warning thresholds')
        if not 0 < unreadable_minutes <= 1440:
            raise ValueError('Invalid unreadable-account warning delay')
        # Consecutive failed reads; kept in memory (after a restart the delay starts again,
        # while an already open KRAKEN_UNREADABLE stays open in alert_state).
        self.unreadable_seconds = unreadable_minutes * 60
        self.failing_since = None
        self.failure_cause = None
        self.failed_reads = 0
        # Heartbeat: last sweep in which the monitor itself worked and published.
        self.last_monitor_success = None
        self.next_account_log = 0.0
        # Not bound to the account hash: Kraken may be unreadable before the first good read.
        self.unreadable_alert_id = ('kraken-account:' + digest({'scope': scope, 'environment': environment,
                                    'policy': policy}) + ':' + UNREADABLE)
        if retention_days is not None and retention_days < 2:
            raise ValueError('Keep at least two days of account samples')
        # Raw account samples (one every poll) older than this are deleted. Alert state,
        # alert events and the per-day portfolio snapshots are small and kept.
        self.retention_seconds = retention_days * 86400 if retention_days else None
        self.last_prune = 0.0
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
                                              self.decline_fraction, self.gross_limit, self.maintenance_drop)
        desired.update(extra)
        timestamp = result['observed_at']
        with self.db:
            previous = self.db.execute('SELECT observed_at FROM alert_clock WHERE id=1').fetchone()
            if previous and timestamp < previous[0]:
                raise JournalError('Out-of-order alert observation')
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
                # A good read is never "desired" to be unreadable, so this resolves it.
                alert_id = (self.unreadable_alert_id if name == UNREADABLE else
                            'kraken-account:' + result['account_identity_hash'] + ':' + name)
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
        self.note_good_read()
        self.prune(timestamp)
        self.publish_account(result, market, now)
        self.publish(now)
        return result

    def note_good_read(self):
        if self.failing_since is not None:
            logging.info('Kraken account read recovered after %d failed reads (failing since %s)',
                         self.failed_reads, utc_minute(self.failing_since))
        self.failing_since, self.failure_cause, self.failed_reads = None, None, 0

    def note_failed_read(self, now, cause):
        """Log the cause when it changes; after the delay open KRAKEN_UNREADABLE once.

        Never touches the other alerts: only a good read may change or resolve them.
        """
        self.failed_reads += 1
        if self.failing_since is None:
            self.failing_since = now
            logging.warning('Kraken account read failed: %s', cause)
        elif cause != self.failure_cause:
            logging.warning('Kraken account read still failing, now: %s', cause)
        self.failure_cause = cause
        if now - self.failing_since < self.unreadable_seconds:
            return
        with self.db:
            state = self.db.execute('SELECT active, sequence FROM alert_state WHERE id=?', (UNREADABLE,)).fetchone()
            if state and state[0]:
                return
            sequence = (state[1] if state else 0) + 1
            last = self.db.execute('SELECT observed_at FROM alert_clock WHERE id=1').fetchone()
            title = 'Kraken account unreadable'
            detail = ('Failing since: ' + utc_minute(self.failing_since) + '\nCause: ' + cause +
                      '\nLast good read: ' + (utc_minute(last[0]) if last else 'none'))
            event = {'eventId': self.unreadable_alert_id + ':' + str(sequence), 'recordedAt': utc(now),
                'transition': 'OPENED', 'alert': {'id': self.unreadable_alert_id, 'timestamp': utc(now),
                'severity': 'WARN', 'status': 'ACTIVE', 'service': 'KrakenAccountReadOnly',
                'account': self.account_name, 'eventType': UNREADABLE, 'title': title, 'detail': detail}}
            self.db.execute('INSERT INTO alert_events(payload) VALUES (?)', (json.dumps(event),))
            self.db.execute('INSERT OR REPLACE INTO alert_state VALUES (?,?,?,?,?,?)',
                            (UNREADABLE, 1, sequence, 'WARN', title, detail))
        logging.warning('Kraken account unreadable for %s minutes; warning opened',
                        format(self.unreadable_seconds / 60, 'g'))

    def publish_account(self, result, market, now):
        """Latest successful observation for the dashboard: collateral, margin, drop to maintenance."""
        totals = result['venue_totals_usd']
        usd = result['currencies'].get('USD')
        document = {'version': 'kraken-account-v1', 'observedAt': utc(result['observed_at']),
                    'accountName': self.account_name, 'equityUsd': totals['marginEquity'],
                    'usdBalance': usd['quantity'] if usd else None,
                    'initialMarginUsd': totals['initialMarginWithOrders'],
                    'positions': len(result['positions']), 'reasons': result['reasons'],
                    **collateral_risk(result, market, now)}
        atomic_text(self.directory / 'account.json', json.dumps(document, indent=2) + '\n')

    def prune(self, observed_at):
        """Delete raw samples beyond the retention window, at most once an hour."""
        if not self.retention_seconds or observed_at - self.last_prune < 3600:
            return
        with self.db:
            self.db.execute('DELETE FROM samples WHERE observed_at < ?', (observed_at - self.retention_seconds,))
        self.last_prune = observed_at

    def publish(self, now, error=''):
        """Events first, then the heartbeat.

        lastSuccessAt is the last sweep in which the monitor itself worked and published,
        whether or not Kraken answered (an unreadable Kraken is the KRAKEN_UNREADABLE alert).
        lastError is set only for the monitor's own failures (journal, storage).
        """
        history = ''.join(row[0] + '\n' for row in self.db.execute('SELECT payload FROM alert_events ORDER BY sequence'))
        atomic_text(self.directory / 'events.jsonl', history)
        if not error:
            self.last_monitor_success = now
        active = list(self.db.execute('SELECT severity FROM alert_state WHERE active=1'))
        heartbeat = {'version': 'step43-v1', 'lastSweepAt': utc(now),
            'lastSuccessAt': utc(self.last_monitor_success) if self.last_monitor_success else '', 'lastError': error,
            'unavailableChecks': self.unavailable_checks,
            'activeCount': len(active), 'activeCritical': sum(row[0] == 'CRITICAL' for row in active),
            'activeWarnings': sum(row[0] == 'WARN' for row in active),
            'eventCount': self.db.execute('SELECT count(*) FROM alert_events').fetchone()[0]}
        atomic_text(self.directory / 'status.json', json.dumps(heartbeat, indent=2) + '\n')

    def poll(self, reader, now=time.time, public_reader=None):
        try:
            bundle = reader.snapshot()
        except Exception as error:
            # No upstream body, URL, credential, exception text or traceback escapes.
            self.note_failed_read(now(), failure_cause(error))
            self.publish(now())
            return None
        market = None
        if self.summary and public_reader and bundle['positions']['openPositions']:
            try:
                market = public_reader.snapshot()
            except Exception:
                pass  # Missing price evidence is shown explicitly in the summary.
        try:
            result = self.observe(bundle, now(), market)
        except (JournalError, sqlite3.Error, OSError):
            # Our own journal/storage: the heartbeat goes stale, which the notifier reports.
            logging.error('Monitor journal or storage refused the observation; recovery not inferred')
            self.publish(now(), 'Monitor journal or storage failed; recovery not inferred')
            return None
        except Exception as error:
            self.note_failed_read(now(), failure_cause(error))  # Kraken answered with invalid data.
            self.publish(now())
            return None
        self.poll_account_log(reader, now())
        return result

    def poll_account_log(self, reader, now):
        """At start and then hourly: summarize the account history for checking the gateway parser.

        Separate from the snapshot: a failure here only retries later and opens no alert.
        """
        if now < self.next_account_log:
            return
        try:
            rows = reader.account_log()
            atomic_text(self.directory / 'account-log-summary.json',
                        json.dumps(account_log_summary(rows, now), indent=2) + '\n')
            self.next_account_log = now + ACCOUNT_LOG_INTERVAL_SECONDS
            logging.info('Kraken account history read: %d entries', len(rows))
        except Exception as error:
            self.next_account_log = now + ACCOUNT_LOG_RETRY_SECONDS
            logging.warning('Kraken account history read failed: %s', failure_cause(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--telegram', action='store_true',
                        help='Local PC only: start/reuse a Go Telegram notifier container (the chain runs its own)')
    parser.add_argument('--environment', choices=('live', 'demo'), default='live')
    parser.add_argument('--interval', type=float, default=30)
    parser.add_argument('--account-name', default='Kraken')
    parser.add_argument('--summary-hour-utc', type=int, default=9)
    parser.add_argument('--daily-equity-drop-fraction', default='0.10')
    parser.add_argument('--gross-exposure-warning', default='5')
    parser.add_argument('--maintenance-drop-warning', default='0.25',
                        help='Warn when a fall of BTC and the long positions smaller than this fraction reaches maintenance margin')
    parser.add_argument('--unreadable-warning-minutes', type=float, default=10,
                        help='Warn (KRAKEN_UNREADABLE) after the account cannot be read for this many minutes')
    parser.add_argument('--policy', type=Path, default=ROOT / 'storage/kraken_shadow/account-policy.json')
    parser.add_argument('--credentials', type=Path, default=ROOT / 'storage/kraken_shadow/.env',
                        help='File with KRAKEN_API_KEY and KRAKEN_API_SECRET (read-only key); read internally, never shown')
    parser.add_argument('--state-root', type=Path, default=ROOT / 'storage/kraken_shadow',
                        help='Folder that must contain --output (an installed chain uses <root>/data/kraken_account)')
    parser.add_argument('--output', type=Path, default=ROOT / 'storage/kraken_shadow/account-alerts')
    parser.add_argument('--retention-days', type=int, default=0,
                        help='Days of raw account samples to keep; 0 keeps all (the chain uses 3)')
    args = parser.parse_args()
    if not 0 <= args.summary_hour_utc <= 23 or not args.account_name.strip() or len(args.account_name) > 64 or any(ord(c) < 32 for c in args.account_name):
        parser.error('Use a single-line account name and a summary UTC hour from 0 to 23')
    directory = args.output.resolve()
    if not directory.is_relative_to(args.state_root.resolve()) or not 10 <= args.interval <= 60:
        parser.error('Keep the output under --state-root and use a polling interval between 10 and 60 seconds')
    if args.retention_days and args.retention_days < 2:
        parser.error('Keep at least two days of account samples (the daily summary compares with yesterday)')
    directory.mkdir(parents=True, exist_ok=True)
    lock = (directory / 'monitor.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        logging.error('Another process owns this output directory')
        raise SystemExit(1) from None
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    signal.signal(signal.SIGINT, lambda *_: stopped.set())
    monitor = None
    try:
        policy = json.loads(args.policy.read_text(encoding='utf-8'))
        reader = reader_from_credentials(args.credentials, args.environment)
        monitor = AccountAlertMonitor(directory, policy, args.environment,
            account_name=args.account_name.strip(), summary_hour_utc=args.summary_hour_utc,
            decline_fraction=args.daily_equity_drop_fraction, gross_limit=args.gross_exposure_warning,
            maintenance_drop=args.maintenance_drop_warning,
            retention_days=args.retention_days or None,
            unreadable_minutes=args.unreadable_warning_minutes)
        public_reader = KrakenPublicReader(args.environment, directory / 'public-prices')
        atomic_text(directory / 'monitor.pid', str(os.getpid()) + '\n')
        notifier_started = False
        while not stopped.is_set():
            result = monitor.poll(reader, public_reader=public_reader)
            if result is not None and args.telegram and not notifier_started:
                start_notifier(directory)
                notifier_started = True
            if args.once:
                if result:
                    logging.info('Kraken read-only observation completed')
                else:
                    logging.warning('Kraken observation failed safely')
                return 0 if result else 1
            stopped.wait(args.interval)
    except Exception:
        # Invalidate any prior fresh source on startup or notifier/persistence failure.
        atomic_text(directory / 'status.json', json.dumps({'version': 'step43-v1',
            'lastSuccessAt': '', 'lastSweepAt': utc(time.time()),
            'lastError': 'Monitor initialization or persistence failed'}))
        logging.error('Monitor initialization or persistence failed; check local policy and credential setup')
        raise SystemExit(1) from None
    finally:
        if monitor:
            monitor.close()
        (directory / 'monitor.pid').unlink(missing_ok=True)
        lock.close()


if __name__ == '__main__':
    setup_logging()
    raise SystemExit(main())
