#!/usr/bin/env python3
"""Offline warning transition/restart/failure acceptance; no real keys or messages."""
import json
from datetime import datetime, timezone
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'live_trading'))
from kraken_account_monitor import AccountAlertMonitor
from kraken_account_test import ACCOUNT_LOG, fixture, RELATIVE_POLICY
from kraken_account import AccountReadError, FIXTURE_SCOPE
from kraken_shadow import SCOPE


class Reader:
    """Stands in for KrakenAccountReader: a snapshot or an error, and the account history."""

    def __init__(self, bundle=None, error=None, rows=None):
        self.bundle, self.error, self.rows = bundle, error, rows
        self.log_reads = 0

    def snapshot(self):
        if self.error:
            raise self.error
        return self.bundle

    def account_log(self):
        self.log_reads += 1
        if self.rows is None:
            raise AccountReadError('Kraken HTTP 503 at account_log')
        return self.rows


class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
        self.clock = time.time()

    def tearDown(self):
        self.monitor.close()
        self.temp.cleanup()

    def bundle(self, low=True):
        self.clock += 1
        bundle = fixture(self.clock)
        if low:
            bundle['wallets']['accounts']['flex']['currencies']['USD'].update(quantity='10', available='10')
        return bundle

    def events(self):
        return [json.loads(line) for line in (self.directory / 'events.jsonl').read_text().splitlines()]

    def observe(self, low=True):
        bundle = self.bundle(low)
        return self.monitor.observe(bundle, self.clock)

    def test_one_grouped_warning_then_recovery_and_recurrence(self):
        self.observe()
        self.observe()
        self.assertEqual(len(self.events()), 1)
        self.assertEqual(self.events()[0]['alert']['eventType'], 'USD_RESERVE')
        self.observe(False)
        self.observe(False)
        self.observe()
        self.assertEqual([e['transition'] for e in self.events()], ['OPENED', 'RESOLVED', 'OPENED'])
        self.assertEqual(len({e['eventId'] for e in self.events()}), 3)

    def test_retention_prunes_old_samples_but_keeps_alerts_and_days(self):
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE, retention_days=2)
        self.observe()
        self.clock += 3 * 86400  # three days later
        self.observe()
        samples = self.monitor.db.execute('SELECT count(*) FROM samples').fetchone()[0]
        days = self.monitor.db.execute('SELECT count(*) FROM portfolio_days').fetchone()[0]
        self.assertEqual(samples, 1)  # the three-day-old raw sample is gone
        self.assertEqual(days, 2)     # per-day portfolio snapshots stay
        self.assertEqual(len(self.events()), 1)  # the active warning is not re-sent
        with self.assertRaises(ValueError):
            AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE, retention_days=1)

    def test_restart_keeps_active_warning_without_duplicate(self):
        self.observe()
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
        self.observe()
        self.assertEqual(len(self.events()), 1)

    def status(self):
        return json.loads((self.directory / 'status.json').read_text())

    def fail_for(self, seconds, reader=None, step=30):
        """Failed polls every `step` seconds for `seconds` (the clock moves with them)."""
        reader = reader or Reader(error=AccountReadError('Kraken HTTP 503 at wallets'))
        end = self.clock + seconds
        while self.clock < end:
            self.clock += step
            self.assertIsNone(self.monitor.poll(reader, lambda: self.clock))

    def test_failure_keeps_other_alerts_and_a_fresh_heartbeat_without_error(self):
        self.observe()
        reader = Reader(error=ValueError('synthetic sensitive upstream error'))
        self.assertIsNone(self.monitor.poll(reader, lambda: self.clock + 1))
        status = self.status()
        # Contract: Kraken failing is not a monitor failure.
        self.assertEqual(status['lastError'], '')
        self.assertEqual(status['lastSuccessAt'], status['lastSweepAt'])
        self.assertNotIn('sensitive', json.dumps(status))
        self.assertEqual(status['activeCount'], 1)
        self.assertEqual(len(self.events()), 1)
        self.observe(False)
        self.assertEqual(self.events()[-1]['transition'], 'RESOLVED')

    def test_unreadable_warning_after_ten_minutes_once_then_resolved(self):
        self.observe()  # USD_RESERVE active before Kraken stops answering
        self.fail_for(9 * 60 + 29)
        self.assertEqual(len(self.events()), 1)  # less than 10 minutes: no warning
        self.fail_for(60)
        unreadable = [e for e in self.events() if e['alert']['eventType'] == 'KRAKEN_UNREADABLE']
        self.assertEqual(len(unreadable), 1)
        alert = unreadable[0]['alert']
        self.assertEqual((alert['severity'], alert['title']), ('WARN', 'Kraken account unreadable'))
        self.assertIn('Cause: Kraken HTTP 503 at wallets', alert['detail'])
        self.assertIn('Failing since: ', alert['detail'])
        self.assertIn('Last good read: ', alert['detail'])
        self.fail_for(30 * 60)
        self.assertEqual(len(self.events()), 2)  # opened once; USD_RESERVE untouched
        self.assertEqual(self.status()['activeCount'], 2)
        self.observe()  # a good read, USD still low
        resolved = self.events()[-1]
        self.assertEqual((resolved['transition'], resolved['alert']['id']), ('RESOLVED', alert['id']))
        self.assertEqual(resolved['alert']['eventType'], 'KRAKEN_UNREADABLE')
        self.assertEqual([e['alert']['eventType'] for e in self.events()].count('USD_RESERVE'), 1)
        self.fail_for(11 * 60)  # a new outage opens a new warning
        reopened = self.events()[-1]
        self.assertEqual((reopened['transition'], reopened['eventId']), ('OPENED', alert['id'] + ':3'))

    def test_unreadable_before_any_good_read_and_restart_without_duplicate(self):
        self.fail_for(11 * 60)
        self.assertEqual(len(self.events()), 1)
        self.assertIn('Last good read: none', self.events()[0]['alert']['detail'])
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
        self.fail_for(20 * 60)
        self.assertEqual(len(self.events()), 1)
        self.observe(False)
        self.assertEqual([e['transition'] for e in self.events()], ['OPENED', 'RESOLVED'])
        self.assertEqual(self.events()[0]['alert']['id'], self.events()[1]['alert']['id'])

    def test_cause_logged_only_when_it_changes_and_on_recovery(self):
        with self.assertLogs(level='INFO') as logs:
            self.fail_for(90)
            self.fail_for(60, Reader(error=AccountReadError('Kraken timeout at permissions')))
            self.observe(False)
        lines = [record.getMessage() for record in logs.records]
        self.assertEqual(lines, ['Kraken account read failed: Kraken HTTP 503 at wallets',
                                 'Kraken account read still failing, now: Kraken timeout at permissions',
                                 'Kraken account read recovered after 5 failed reads (failing since ' +
                                 lines[2].split('since ')[1]])

    def test_monitor_journal_failure_sets_last_error_and_stops_success(self):
        self.observe()
        before = self.status()['lastSuccessAt']
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
        self.clock -= 10  # older than the last stored sample: our journal refuses it
        self.assertIsNone(self.monitor.poll(Reader(bundle=fixture(self.clock)), lambda: self.clock))
        status = self.status()
        self.assertEqual(status['lastError'], 'Monitor journal or storage failed; recovery not inferred')
        self.assertEqual(status['lastSuccessAt'], '')  # stale: the notifier reports a stopped monitor
        self.assertNotEqual(before, '')
        self.assertFalse(any(e['alert']['eventType'] == 'KRAKEN_UNREADABLE' for e in self.events()))

    def test_account_log_summary_hourly_and_its_failure_changes_nothing_else(self):
        reader = Reader(rows=ACCOUNT_LOG)
        self.clock += 1
        reader.bundle = fixture(self.clock)
        self.assertIsNotNone(self.monitor.poll(reader, lambda: self.clock))
        summary = json.loads((self.directory / 'account-log-summary.json').read_text())
        self.assertEqual((summary['entries'], reader.log_reads), (4, 1))
        self.assertNotIn('9001', json.dumps(summary))
        for _ in range(3):  # within the hour: not read again
            self.clock += 30
            reader.bundle = fixture(self.clock)
            self.monitor.poll(reader, lambda: self.clock)
        self.assertEqual(reader.log_reads, 1)
        self.clock += 3600
        reader.bundle, reader.rows = fixture(self.clock), None  # history read now fails
        with self.assertLogs(level='WARNING') as logs:
            self.assertIsNotNone(self.monitor.poll(reader, lambda: self.clock))
        self.assertIn('Kraken account history read failed: Kraken HTTP 503 at account_log', logs.output[0])
        self.assertEqual(self.status()['lastError'], '')
        self.assertFalse(any(e['alert']['eventType'] == 'KRAKEN_UNREADABLE' for e in self.events()))
        self.assertEqual(json.loads((self.directory / 'account-log-summary.json').read_text())['entries'], 4)
        self.clock += 300  # retried after five minutes
        reader.bundle, reader.rows = fixture(self.clock), ACCOUNT_LOG[:2]
        self.monitor.poll(reader, lambda: self.clock)
        self.assertEqual(json.loads((self.directory / 'account-log-summary.json').read_text())['entries'], 2)

    def test_stale_invalid_observation_cannot_resolve(self):
        self.observe()
        bundle = self.bundle(False)
        with self.assertRaises(ValueError):
            self.monitor.observe(bundle, self.clock + 60)
        self.assertEqual(len(self.events()), 1)

    def test_distinct_critical_margin_alerts(self):
        bundle = self.bundle()
        wallet = bundle['wallets']['accounts']['flex']
        wallet.update(initialMarginWithOrders='8000', maintenanceMargin='7000')
        self.monitor.observe(bundle, self.clock)
        events = self.events()
        self.assertEqual(len(events), 3)
        self.assertEqual(sum(e['alert']['severity'] == 'CRITICAL' for e in events), 2)

    def test_account_floor_warning(self):
        bundle = self.bundle()
        bundle['wallets']['accounts']['flex'].update(marginEquity='49', availableMargin='0',
            initialMargin='0', initialMarginWithOrders='0', maintenanceMargin='0', totalUnrealized='0')
        self.monitor.observe(bundle, self.clock)
        self.assertEqual(self.events()[0]['alert']['eventType'], 'ACCOUNT_EQUITY_BELOW_MINIMUM')

    def test_policy_change_rejected_before_transition(self):
        self.observe()
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, {**RELATIVE_POLICY, 'minimum_account_equity_usd': '51'},
                                          scope=FIXTURE_SCOPE)
        with self.assertRaises(ValueError):
            self.observe(False)
        self.assertEqual(len(self.events()), 1)

    def test_committed_history_can_be_republished(self):
        self.observe()
        (self.directory / 'events.jsonl').write_text('')
        self.monitor.publish(self.clock)
        self.assertEqual(len(self.events()), 1)

    def account_sample(self, day, equity, quantity='0.01', mark='50000', btc_collateral=False):
        self.clock = datetime(2026, 10, day, 12, tzinfo=timezone.utc).timestamp()
        bundle = fixture(self.clock)
        bundle['wallets']['accounts']['flex']['marginEquity'] = equity
        bundle['positions']['openPositions'][0]['size'] = quantity
        if not btc_collateral:
            # These tests are about other warnings: no BTC, so with only a short position a
            # fall cannot reach maintenance margin.
            bundle['wallets']['accounts']['flex']['currencies']['XBT'].update(
                quantity='0', value='0', collateral='0', available='0')
        market = {'scope': SCOPE, 'environment': 'live', 'complete': True,
            'observed_at': self.clock, 'server_times': [self.clock, self.clock], 'markets': {'BTC': {
                'symbol': 'PF_XBTUSD', 'trading': True, 'quote_currency': 'USD', 'mark_price': mark}}}
        return bundle, market

    def test_ten_percent_daily_equity_drop_warns_without_a_stop(self):
        bundle, market = self.account_sample(10, '1000')
        self.monitor.observe(bundle, self.clock, market)
        bundle, market = self.account_sample(11, '900')
        result = self.monitor.observe(bundle, self.clock, market)
        daily = [e for e in self.events() if e['alert']['eventType'] == 'DAILY_EQUITY_DROP']
        self.assertEqual(len(daily), 1)
        self.assertEqual(daily[0]['alert']['severity'], 'WARN')
        self.assertIn('10.00%', daily[0]['alert']['detail'])
        self.assertFalse(result['submitted'])
        self.assertIsNone(result['can_trade'])
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(len(self.events()), 1)

    def test_daily_comparison_is_not_a_peak_drawdown(self):
        for day, equity in ((10, '1000'), (11, '950'), (12, '900')):
            bundle, market = self.account_sample(day, equity)
            self.monitor.observe(bundle, self.clock, market)
        self.assertFalse(any(e['alert']['eventType'] == 'DAILY_EQUITY_DROP' for e in self.events()))

    def test_missing_yesterday_does_not_invent_a_drop_or_recovery(self):
        for day, equity in ((10, '1000'), (11, '900'), (13, '1000')):
            bundle, market = self.account_sample(day, equity)
            self.monitor.observe(bundle, self.clock, market)
        daily = [e for e in self.events() if e['alert']['eventType'] == 'DAILY_EQUITY_DROP']
        self.assertEqual([e['transition'] for e in daily], ['OPENED'])

    def test_gross_exposure_five_x_boundary_and_missing_prices(self):
        bundle, market = self.account_sample(10, '1000', quantity='0.1')
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(self.events(), [])  # Exactly 5x is within the warning threshold.
        bundle, market = self.account_sample(11, '1000', quantity='0.101')
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(self.events()[0]['alert']['eventType'], 'GROSS_EXPOSURE')
        self.assertIn('5.05x', self.events()[0]['alert']['detail'])
        bundle, _ = self.account_sample(12, '1000')
        self.monitor.observe(bundle, self.clock, None)
        self.assertEqual(len(self.events()), 1)  # Unknown price is not exposure recovery.
        status = json.loads((self.directory / 'status.json').read_text())
        self.assertIn('GROSS_EXPOSURE', status['unavailableChecks'])
        bundle, market = self.account_sample(13, '1000')
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(self.events()[-1]['transition'], 'RESOLVED')

    def test_maintenance_drop_below_25_percent_warns_and_resolves(self):
        # BTC collateral 4950 after haircut, a 500 USD short: long exposure 4450.
        # Maintenance margin 250, so equity 1362.5 is exactly a 25% fall away.
        bundle, market = self.account_sample(10, '1362.5', btc_collateral=True)
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(self.events(), [])
        bundle, market = self.account_sample(11, '1350', btc_collateral=True)
        self.monitor.observe(bundle, self.clock, market)
        alert = self.events()[0]['alert']
        self.assertEqual((alert['eventType'], alert['severity']), ('MAINTENANCE_DROP', 'WARN'))
        self.assertIn('A fall of 24.7% in BTC and the long positions', alert['detail'])
        self.assertIn('BTC alone: 22.2%', alert['detail'])
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(len(self.events()), 1)  # sent once
        bundle, _ = self.account_sample(12, '2000', btc_collateral=True)
        self.monitor.observe(bundle, self.clock, None)
        self.assertEqual(len(self.events()), 1)  # no prices: unknown, not a recovery
        status = json.loads((self.directory / 'status.json').read_text())
        self.assertIn('MAINTENANCE_DROP', status['unavailableChecks'])
        bundle, market = self.account_sample(13, '2000', btc_collateral=True)
        self.monitor.observe(bundle, self.clock, market)
        self.assertEqual(self.events()[-1]['transition'], 'RESOLVED')
        with self.assertRaises(ValueError):
            AccountAlertMonitor(self.directory / 'other', RELATIVE_POLICY, scope=FIXTURE_SCOPE, maintenance_drop='0')

    @unittest.skipUnless(os.environ.get('KRAKEN_MONITOR_DOCKER_TEST') == '1', 'optional Go notifier contract acceptance (image: KRAKEN_MONITOR_DOCKER_IMAGE, default the chain dash-api image)')
    def test_existing_notifier_contract_and_restart(self):
        # TEST_FILE only, network disabled and no credential file passed to Docker.
        with tempfile.TemporaryDirectory(dir=ROOT / 'storage', prefix='kraken-alert-contract-') as location:
            directory = Path(location)
            # A short unreadable delay so the real notifier sees KRAKEN_UNREADABLE in seconds.
            monitor = AccountAlertMonitor(directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE, unreadable_minutes=0.01)
            name = 'kraken-alert-contract-' + str(os.getpid())
            def docker(*args):
                return subprocess.run(['docker', *args], capture_output=True, text=True, check=True, timeout=30).stdout
            def wait_delivered(count):
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    status = subprocess.run(['docker', 'exec', name, 'cat', '/data/notifier/status.json'],
                                            capture_output=True, text=True, timeout=10)
                    if status.returncode == 0 and json.loads(status.stdout).get('deliveredCount') == count:
                        return
                    time.sleep(0.5)
                self.fail('Go notifier did not accept the expected transition count')
            try:
                bundle = self.bundle()
                monitor.observe(bundle, self.clock)
                docker('run', '-d', '--name', name, '--network', 'none',
                    '-v', str(directory) + ':/source:ro',
                    '-e', 'DASHBOARD_NOTIFIER_SINK=TEST_FILE',
                    '-e', 'DASHBOARD_ALERT_STORE_DIR=/source',
                    '-e', 'DASHBOARD_NOTIFIER_INTERVAL=1s',
                    '--entrypoint', '/dashboard-alert-notifier',
                    os.environ.get('KRAKEN_MONITOR_DOCKER_IMAGE', 'algotrading-dash-api:chain'))
                wait_delivered(1)
                docker('restart', name)
                wait_delivered(1)
                monitor.observe(self.bundle(False), self.clock)
                wait_delivered(2)
                docker('restart', name)
                wait_delivered(2)
                # Kraken failing keeps a fresh heartbeat without lastError, so the notifier
                # delivers the monitor's own warning (on 2026-10-15 nothing was sent).
                failing = Reader(error=AccountReadError('Kraken HTTP 503 at wallets'))
                monitor.poll(failing)
                time.sleep(1)
                monitor.poll(failing)
                self.assertEqual(json.loads((directory / 'events.jsonl').read_text().splitlines()[-1])
                                 ['alert']['eventType'], 'KRAKEN_UNREADABLE')
                wait_delivered(3)
                self.clock = time.time()
                monitor.observe(self.bundle(False), self.clock)
                wait_delivered(4)
            finally:
                subprocess.run(['docker', 'rm', '-f', name], capture_output=True, timeout=30)
                monitor.close()


if __name__ == '__main__':
    unittest.main()
