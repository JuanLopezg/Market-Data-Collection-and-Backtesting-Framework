#!/usr/bin/env python3
"""Daily USD summary acceptance: source valuation, calendar timing and restart."""
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'live_trading'))
from kraken_account_monitor import AccountAlertMonitor
from kraken_account import FIXTURE_SCOPE
from kraken_portfolio_summary import dollars, position_lines
from kraken_shadow import SCOPE
from kraken_account_test import fixture, RELATIVE_POLICY


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE,
                                          account_name='Kraken', summary_hour_utc=9)

    def tearDown(self):
        self.monitor.close()
        self.temp.cleanup()

    def observe(self, day, hour, empty=False, mark='60000'):
        now = datetime(2026, 10, day, hour, tzinfo=timezone.utc).timestamp()
        bundle = fixture(now)
        if empty:
            bundle['positions']['openPositions'] = []
        market = {'scope': SCOPE, 'environment': 'live', 'complete': True,
            'observed_at': now, 'server_times': [now, now], 'markets': {'BTC': {
                'symbol': 'PF_XBTUSD', 'trading': True, 'quote_currency': 'USD', 'mark_price': mark}}}
        result = self.monitor.observe(bundle, now, market)
        return result, market, now

    def summaries(self):
        return [json.loads(row[0]) for row in self.monitor.db.execute('SELECT payload FROM alert_events')
                if json.loads(row[0])['alert']['eventType'] == 'DAILY_PORTFOLIO']

    def test_daily_schedule_usd_mark_and_missing_yesterday(self):
        self.assertEqual(dollars('-25'), '-$25.00')
        self.observe(10, 8)
        self.assertEqual(self.summaries(), [])
        self.observe(10, 9)
        summary = self.summaries()[0]['alert']
        self.assertEqual(summary['account'], 'Kraken')
        self.assertEqual(summary['severity'], 'INFO')
        self.assertIn('Balance: $7,000.00', summary['detail'])
        self.assertIn('Equity: $6,849.00', summary['detail'])
        self.assertIn('BTC SHORT: $600.00 (8.76% equity)', summary['detail'])
        self.assertIn('unavailable (no snapshot)', summary['detail'])
        self.observe(10, 10)
        self.assertEqual(len(self.summaries()), 1)

    def test_yesterday_uses_last_snapshot_and_restart_is_deduplicated(self):
        self.observe(10, 9, mark='60000')
        self.observe(10, 23, mark='70000')
        self.monitor.close()
        self.monitor = AccountAlertMonitor(self.directory, RELATIVE_POLICY, scope=FIXTURE_SCOPE,
                                          summary_hour_utc=9)
        self.observe(11, 9, empty=True)
        summary = self.summaries()[-1]
        self.assertEqual(summary['transition'], 'UPDATED')
        self.assertIn('Active positions (USD exposure):\nNone', summary['alert']['detail'])
        self.assertIn('2026-10-10 23:00 UTC', summary['alert']['detail'])
        self.assertIn('BTC SHORT: $700.00 (10.22% equity)', summary['alert']['detail'])
        self.observe(11, 10)
        self.assertEqual(len(self.summaries()), 2)

    def test_missing_day_is_not_backfilled_or_called_empty(self):
        self.observe(10, 9)
        self.observe(12, 9)
        self.assertEqual(len(self.summaries()), 2)
        self.assertIn('unavailable (no snapshot)', self.summaries()[-1]['alert']['detail'])

    def test_missing_stale_prices_never_use_entry_price_as_current_value(self):
        result, market, now = self.observe(10, 9)
        self.assertIn('unavailable', position_lines(result, None, now)[0])
        result['venue_totals_usd']['marginEquity'] = '0'
        self.assertIn('equity % unavailable', position_lines(result, market, now)[0])
        result['venue_totals_usd']['marginEquity'] = '-100'
        self.assertIn('equity % unavailable', position_lines(result, market, now)[0])
        result['venue_totals_usd']['marginEquity'] = '100'
        self.assertIn('600.00% equity', position_lines(result, market, now)[0])
        market['observed_at'] -= 90
        self.assertIn('unavailable', position_lines(result, market, now)[0])

    def test_stale_account_does_not_create_daily_summary(self):
        now = datetime(2026, 10, 10, 9, tzinfo=timezone.utc).timestamp()
        with self.assertRaises(ValueError):
            self.monitor.observe(fixture(now), now + 60)
        self.assertEqual(self.summaries(), [])


if __name__ == '__main__':
    unittest.main()
