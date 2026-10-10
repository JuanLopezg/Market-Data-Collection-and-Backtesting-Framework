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
from kraken_account_test import fixture, RELATIVE_POLICY
from kraken_account import FIXTURE_SCOPE
from kraken_shadow import SCOPE


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

    def test_restart_keeps_active_warning_without_duplicate(self):
        self.observe()
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
        self.observe()
        self.assertEqual(len(self.events()), 1)

    def test_failure_retains_active_alert_and_blocks_fresh_heartbeat(self):
        self.observe()
        class FailedReader:
            def snapshot(self):
                raise ValueError('synthetic sensitive upstream error')
        self.assertIsNone(self.monitor.poll(FailedReader(), lambda: self.clock + 1))
        status = json.loads((self.directory / 'status.json').read_text())
        self.assertTrue(status['lastError'])
        self.assertNotIn('sensitive', status['lastError'])
        self.assertEqual(status['activeCount'], 1)
        self.assertEqual(len(self.events()), 1)
        self.observe(False)
        self.assertEqual(self.events()[-1]['transition'], 'RESOLVED')

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

    def account_sample(self, day, equity, quantity='0.01', mark='50000'):
        self.clock = datetime(2026, 10, day, 12, tzinfo=timezone.utc).timestamp()
        bundle = fixture(self.clock)
        bundle['wallets']['accounts']['flex']['marginEquity'] = equity
        bundle['positions']['openPositions'][0]['size'] = quantity
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

    @unittest.skipUnless(os.environ.get('KRAKEN_MONITOR_DOCKER_TEST') == '1', 'optional local Go notifier contract acceptance')
    def test_existing_notifier_contract_and_restart(self):
        # TEST_FILE only, network disabled and no credential file passed to Docker.
        with tempfile.TemporaryDirectory(dir=ROOT / 'storage', prefix='kraken-alert-contract-') as location:
            directory = Path(location)
            monitor = AccountAlertMonitor(directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE)
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
                    '--entrypoint', '/dashboard-alert-notifier', 'algotrading-telegram-api')
                wait_delivered(1)
                docker('restart', name)
                wait_delivered(1)
                monitor.observe(self.bundle(False), self.clock)
                wait_delivered(2)
                docker('restart', name)
                wait_delivered(2)
            finally:
                subprocess.run(['docker', 'rm', '-f', name], capture_output=True, timeout=30)
                monitor.close()


if __name__ == '__main__':
    unittest.main()
