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
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'deploy/chain/comparison.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


BINARY = ROOT / 'build/research/src/legacy/algotrading_research'
VOL_TARGET = ROOT / 'config/portfolio/pure_rsi_vol_target.json'
EQUAL_WEIGHT = ROOT / 'config/portfolio/pure_rsi_equal_weight.json'


def market_database(output, wiggle=False):
    """100 completed days of BTCUSDT closing at 100..199, plus a poisoned future bar."""
    database = output / 'market.sqlite'
    end = date(2026, 10, 9)
    with sqlite3.connect(database) as db:
        db.executescript('''CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,close REAL,volume REAL,quote_volume REAL);
          CREATE TABLE market_volume_rank_daily(date INTEGER,rank INTEGER,pair TEXT,quote_volume REAL);''')
        for index in range(100):
            day = int((end - timedelta(days=99-index)).strftime('%Y%m%d'))
            # A small alternating move gives the volatility target a nonzero variance.
            price = 100 + index + (3 if wiggle and index % 2 else 0)
            db.execute('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?,?)', ('BTCUSDT', day, price, price, price, price, 100, price*100))
        # Poisoned future bar cannot influence a decision on the preceding close.
        db.execute('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?,?)', ('BTCUSDT', 20261010, 400, 999999, 1, 1, 999999, 999999))
        db.execute('INSERT INTO market_volume_rank_daily VALUES(?,?,?,?)', (20261009, 1, 'BTCUSDT', 19900))
    return database


def run_baseline(output, portfolio=None, start=None):
    manifest = {'strategy_config': str(comparison.PROFILE), 'portfolio_config': str(portfolio or comparison.PORTFOLIO),
                'initial_cash': 100000, 'commission_rate': .001, 'warmup_days': 100,
                'cycles': [{'database': str(output / 'market.sqlite'), 'decision_date': 20261009,
                            'execution_date': 20261010, 'open_prices': {'BTCUSDT': 400}}],
                'output': str(output / 'result.json')}
    if start:
        manifest['start'] = start
    comparison.write_json(output / 'manifest.json', manifest)
    env = dict(os.environ, ALGOTRADING_PAPER_BASELINE_MANIFEST=str(output / 'manifest.json'))
    subprocess.run([str(BINARY)], env=env, check=True, capture_output=True)
    return json.loads((output / 'result.json').read_text())


class PaperBaselineTest(unittest.TestCase):
    def test_warmup_next_open_fees_restart_and_no_future_close(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            market_database(output)
            def run():
                return run_baseline(output, EQUAL_WEIGHT)
            result = run()
            row = result['rows'][0]
            self.assertEqual(len(result['rows']), 1)
            self.assertAlmostEqual(row['positions']['BTCUSDT'], 25)
            self.assertAlmostEqual(row['cash'], 89990)
            self.assertAlmostEqual(row['equity'], 99990)
            self.assertEqual(row['signals']['BTCUSDT'], 1)
            self.assertEqual(result, run())

    def test_volatility_target_portfolio_is_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            market_database(output, wiggle=True)
            result = run_baseline(output, VOL_TARGET)
            row = result['rows'][0]
            self.assertEqual(row['signals']['BTCUSDT'], 1)
            quantity = row['positions']['BTCUSDT']
            # Sized by volatility, capped by the 1.5x asset/gross limit: positive, at most 1.5 x equity.
            self.assertGreater(quantity, 0)
            self.assertLessEqual(quantity * 400, 1.5 * 100000 + 1e-6)
            self.assertNotAlmostEqual(quantity, 25)  # not the 10% equal-weight size
            self.assertEqual(result, run_baseline(output, VOL_TARGET))

    def test_seeded_start_continues_from_the_paper_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            market_database(output)
            start = {'cash': 50000, 'positions': {'BTCUSDT': 10},
                     'strategies': [{'strategy_id': 1, 'signals': {'BTCUSDT': 1}, 'desired_weights': {'BTCUSDT': 0.1},
                                     'virtual_positions': {'BTCUSDT': 10}}]}
            row = run_baseline(output, EQUAL_WEIGHT, start)['rows'][0]
            # Signal still long and entry/exit-only rebalancing: the seeded position is held.
            self.assertAlmostEqual(row['positions']['BTCUSDT'], 10)
            self.assertAlmostEqual(row['cash'], 50000)
            self.assertAlmostEqual(row['equity'], 54000)
            start['strategies'][0]['strategy_id'] = 9
            with self.assertRaises(subprocess.CalledProcessError):
                run_baseline(output, EQUAL_WEIGHT, start)

    def test_configuration_change_archives_and_seeds_a_new_study(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            live = {'account_cash': 1000, 'account_positions': {'BTCUSDT': 2}, 'strategies': []}
            old = {'fingerprint': 'a' * 64, 'cycles': [{'decision_date': 20261012, 'execution_date': 20261013, 'live': live}]}
            comparison.write_json(output / 'study.json', old)
            comparison.write_json(output / 'comparison.json', {'rows': [{'date': '2026-10-13', 'liveEquity': 1}]})
            (output / 'market-20261012.sqlite').write_text('x')
            with self.assertRaises(ValueError):  # the account moved on after the last capture
                comparison.restart_study(output, old, 'b' * 64, {**live, 'account_cash': 999})
            study = comparison.restart_study(output, old, 'b' * 64, live)
            archive = output / ('archive-' + 'a' * 12)
            self.assertTrue((archive / 'study.json').exists() and (archive / 'market-20261012.sqlite').exists())
            self.assertFalse((output / 'comparison.json').exists())
            self.assertEqual(study['seed'], {'decision_date': 20261012, 'execution_date': 20261013, 'live': live})
            self.assertEqual(study['previousRows'], [{'date': '2026-10-13', 'liveEquity': 1}])
            self.assertEqual(json.loads((output / 'study.json').read_text()), study)

    def test_restart_reordering_is_not_an_account_change(self):
        stored = {'account_cash': 1, 'orders': [{'order_id': 2}, {'order_id': 1}], 'processed_fill_ids': [2, 1]}
        after_restart = {'account_cash': 1, 'orders': [{'order_id': 1}, {'order_id': 2}], 'processed_fill_ids': [1, 2]}
        self.assertEqual(comparison.normalized(stored), comparison.normalized(after_restart))
        self.assertNotEqual(comparison.normalized(stored), comparison.normalized({**after_restart, 'account_cash': 2}))

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
