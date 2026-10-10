#!/usr/bin/env python3
"""Coverage calculation regressions; optional audit of a retained public snapshot."""
import argparse
import copy
from decimal import Decimal
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('coverage', ROOT / 'tools/exchange_coverage.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class CoverageTest(unittest.TestCase):
    def test_actual_quote_turnover_not_units_or_close_estimate(self):
        row = [0, '1', '2', '0.5', '100', '1000000000', 86399999, '27.25']
        self.assertEqual(coverage.turnover([row], 0, 86400000, 0), (Decimal('27.25'), 1))

    def test_duplicate_unfinished_and_missing_after_listing_rejected(self):
        row = [0, 0, 0, 0, 0, 0, 86399999, '1']
        for rows in ([row, row], [], [[*row[:6], 86400000, '1']]):
            with self.assertRaises(ValueError):
                coverage.turnover(rows, 0, 86400000, 0)
        self.assertEqual(coverage.turnover([], 0, 86400000, 86400000), (Decimal(0), 0))

    def test_scaled_aliases_do_not_strip_arbitrary_digits_or_merge_neiro(self):
        self.assertEqual(coverage.asset('1000PEPE'), coverage.asset('kPEPE'))
        self.assertEqual(coverage.asset('XBT'), 'BTC')
        self.assertEqual(coverage.asset('1INCH'), '1INCH')
        self.assertNotEqual(coverage.asset('NEIRO'), coverage.asset('NEIROETH'))

    def test_kraken_requires_live_nonexpired_perpetual(self):
        row = {'symbol': 'PF_XBTUSD', 'tradeable': True, 'isExpired': False, 'base': 'BTC'}
        ticker = {'symbol': row['symbol'], 'tag': 'perpetual', 'suspended': False, 'markPrice': 100}
        self.assertEqual(coverage.kraken_catalog({'instruments': [row]}, {'tickers': [ticker]}), {'BTC': ['PF_XBTUSD']})
        for changed in ({'suspended': True}, {'tag': 'quarter'}, {'markPrice': 0}):
            self.assertEqual(coverage.kraken_catalog({'instruments': [row]}, {'tickers': [{**ticker, **changed}]}), {})
        self.assertEqual(coverage.kraken_catalog({'instruments': [{**row, 'isExpired': True}]}, {'tickers': [ticker]}), {})

    def test_hyperliquid_delisted_empty_book_and_context_alignment(self):
        rows = [{'name': 'kPEPE'}, {'name': 'MATIC', 'isDelisted': True}]
        contexts = [{'midPx': '1', 'markPx': '1'}, {'midPx': '1', 'markPx': '1'}]
        self.assertEqual(coverage.hyperliquid_catalog([{'universe': rows}, contexts]), {'PEPE': ['kPEPE']})
        contexts[0]['midPx'] = None
        self.assertEqual(coverage.hyperliquid_catalog([{'universe': rows}, contexts]), {})
        with self.assertRaises(ValueError):
            coverage.hyperliquid_catalog([{'universe': rows}, contexts[:1]])


def audit(directory):
    data = json.loads((directory / 'comparison.json').read_text(encoding='utf-8'))
    raw = directory / 'raw'
    def source(name):
        return json.loads((raw / name).read_text(encoding='utf-8'))['data']
    from datetime import datetime
    start = int(datetime.fromisoformat(data['window_start_utc']).timestamp() * 1000)
    end = int(datetime.fromisoformat(data['window_end_exclusive_utc']).timestamp() * 1000)
    markets = {row['symbol']: row for row in source('binance-markets.json')['symbols']}
    for row in data['all_candidates']:
        symbol = markets[row['binance']]
        assert symbol['underlyingType'] == 'COIN' and symbol['quoteAsset'] == 'USDT' and symbol['status'] == 'TRADING'
        volume, days = coverage.turnover(source(row['binance'] + '.json'), start, end, symbol['onboardDate'])
        assert volume == Decimal(row['quote_volume_usdt']) and days == row['observed_days']
    ranked = sorted(data['all_candidates'], key=lambda row: (-Decimal(row['quote_volume_usdt']), row['binance']))
    selected, seen = [], set()
    for row in ranked:
        if row['asset'] not in seen:
            selected.append(row['binance'])
            seen.add(row['asset'])
        if len(selected) == len(data['assets']):
            break
    assert selected == [row['binance'] for row in data['assets']]
    kraken = coverage.kraken_catalog(source('kraken-instruments.json'), source('kraken-tickers.json'))
    hyper = coverage.hyperliquid_catalog(source('hyperliquid-main.json'))
    builder = {}
    for dex in source('hyperliquid-dexes.json'):
        if dex and dex.get('name'):
            for coin, symbols in coverage.hyperliquid_catalog(source('hyperliquid-' + dex['name'] + '.json')).items():
                builder.setdefault(coin, []).extend(symbols)
    for row in data['assets']:
        assert row['kraken'] == kraken.get(row['asset'], [])
        assert row['hyperliquid_main'] == hyper.get(row['asset'], [])
        assert row['hyperliquid_hip3'] == builder.get(row['asset'], [])
    for name, fields in [('Kraken', ['kraken']), ('Hyperliquid main', ['hyperliquid_main']),
                        ('Hyperliquid including HIP-3', ['hyperliquid_main', 'hyperliquid_hip3'])]:
        count = sum(any(row[field] for field in fields) for row in data['assets'])
        assert count == data['summary'][name]['count']
    print('COVERAGE-AUDIT: PASS: all candles, full ranking, live filters, aliases and three coverage counts')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(CoverageTest))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.snapshot:
        audit(args.snapshot)
