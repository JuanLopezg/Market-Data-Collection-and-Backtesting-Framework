#!/usr/bin/env python3
"""Bounded fast PAPER baseline: warmup, next-open pricing, fees and future exclusion."""
from datetime import date, timedelta
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'deploy/paper_trading/comparison.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class PaperBaselineTest(unittest.TestCase):
    def test_warmup_next_open_fees_restart_and_no_future_close(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            database = output / 'market.sqlite'
            end = date(2026, 10, 9)
            with sqlite3.connect(database) as db:
                db.executescript('''CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,close REAL,volume REAL,quote_volume REAL);
                  CREATE TABLE market_volume_rank_daily(date INTEGER,rank INTEGER,pair TEXT,quote_volume REAL);''')
                for index in range(100):
                    day = int((end - timedelta(days=99-index)).strftime('%Y%m%d'))
                    price = 100 + index
                    db.execute('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?,?)', ('BTCUSDT', day, price, price, price, price, 100, price*100))
                # Poisoned future bar cannot influence a decision on the preceding close.
                db.execute('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?,?)', ('BTCUSDT', 20261010, 400, 999999, 1, 1, 999999, 999999))
                db.execute('INSERT INTO market_volume_rank_daily VALUES(?,?,?,?)', (20261009, 1, 'BTCUSDT', 19900))
            manifest = {'strategy_config': str(comparison.PROFILE), 'portfolio_config': str(comparison.PORTFOLIO),
                        'initial_cash': 100000, 'commission_rate': .001, 'warmup_days': 100,
                        'cycles': [{'database': str(database), 'decision_date': 20261009,
                                    'execution_date': 20261010, 'open_prices': {'BTCUSDT': 400}}],
                        'output': str(output / 'result.json')}
            comparison.write_json(output / 'manifest.json', manifest)
            env = dict(os.environ, ALGOTRADING_PAPER_BASELINE_MANIFEST=str(output / 'manifest.json'))
            binary = ROOT / 'build/research/src/legacy/algotrading_research'
            def run():
                subprocess.run([str(binary)], env=env, check=True, capture_output=True)
                return json.loads((output / 'result.json').read_text())
            result = run()
            row = result['rows'][0]
            self.assertEqual(len(result['rows']), 1)
            self.assertAlmostEqual(row['positions']['BTCUSDT'], 25)
            self.assertAlmostEqual(row['cash'], 89990)
            self.assertAlmostEqual(row['equity'], 99990)
            self.assertEqual(row['signals']['BTCUSDT'], 1)
            self.assertEqual(result, run())

    def test_comparison_rejects_wrong_cycle_and_marks_signed_exposure(self):
        cycles = [{'decision_date': 20261009, 'execution_date': 20261010,
                   'open_prices': {'BTCUSDT': 100}, 'signals': {'BTCUSDT': 1},
                   'live': {'account_cash': 1100, 'account_positions': {'BTCUSDT': -1}}}]
        baseline = {'rows': [{'decisionDate': 20261009, 'executionDate': 20261010,
                             'cash': 900, 'equity': 1000, 'positions': {'BTCUSDT': 1}, 'signals': {}}]}
        rows, positions = comparison.compare(cycles, baseline)
        self.assertEqual(rows[0]['liveEquity'], 1000)
        self.assertEqual(rows[0]['signalDifferences'], 1)
        self.assertEqual(positions[0]['liveUsd'], -100)
        baseline['rows'][0]['executionDate'] = 20261011
        with self.assertRaises(ValueError):
            comparison.compare(cycles, baseline)


if __name__ == '__main__':
    unittest.main()
