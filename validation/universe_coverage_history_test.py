#!/usr/bin/env python3
"""Offline checks of tools/universe_coverage_history.py (no network)."""
from datetime import date, timedelta
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('coverage', ROOT / 'tools/universe_coverage_history.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


def days(count):
    start = date(2026, 9, 1)
    return [(start + timedelta(days=i)).strftime('%Y%m%d') for i in range(count)]


class UniverseCoverageTest(unittest.TestCase):
    def test_universe_needs_full_history_and_reports_missing_with_strength(self):
        calendar = days(40)
        turnover = {'BTCUSDT': {d: 100.0 for d in calendar},
                    'NEWUSDT': {d: 500.0 for d in calendar[10:]},        # eligible from its 25th day
                    'PEPEUSDT': {d: 50.0 for d in calendar},
                    'DOGEUSDT': {d: 10.0 for d in calendar}}
        closes = {'BTCUSDT': {d: 100.0 + i for i, d in enumerate(calendar)},
                  'NEWUSDT': {d: 1.0 for d in calendar[10:]},
                  'PEPEUSDT': {d: 100.0 - i for i, d in enumerate(calendar)},  # falling: RSI 0
                  'DOGEUSDT': {d: 1.0 for d in calendar}}
        venues = {'kraken': lambda s: s == 'BTCUSDT', 'all': lambda s: True}
        result = coverage.analyse(turnover, closes, venues, top_n=2)
        self.assertEqual(result[0]['day'], calendar[24])               # first day with 25 days of turnover
        self.assertEqual(result[0]['universe'], ['BTCUSDT', 'PEPEUSDT'])
        self.assertEqual(result[0]['kraken']['missing'], ['PEPEUSDT'])
        self.assertEqual(result[0]['kraken']['rsi_above_50'], [])         # falling, not a long candidate
        self.assertEqual(result[0]['all']['missing'], [])
        # Once NEWUSDT has 25 days it enters the universe; it is missing and RSI is flat.
        self.assertIn('NEWUSDT', [row for row in result if row['day'] == calendar[-1]][0]['universe'])
        summary = coverage.summarize(result, 'kraken')
        self.assertEqual(summary['days_with_missing'], len(result))
        self.assertEqual(summary['average_missing_per_day'], 1.0)

    def test_rsi_and_base_names(self):
        self.assertEqual(coverage.rsi([1, 2, 3, 4, 5, 6, 7, 8]), 100.0)
        self.assertEqual(coverage.rsi([8, 7, 6, 5, 4, 3, 2, 1]), 0.0)
        self.assertIsNone(coverage.rsi([1, 2, 3]))
        self.assertEqual(coverage.binance_base('1000PEPEUSDT'), 'PEPE')
        self.assertEqual(coverage.binance_base('1000000MOGUSDT'), 'MOG')
        self.assertEqual(coverage.binance_base('1INCHUSDT'), '1INCH')


if __name__ == '__main__':
    unittest.main()
