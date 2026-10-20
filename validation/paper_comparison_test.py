#!/usr/bin/env python3
"""Bounded fast PAPER baseline: warmup, next-open pricing, fees and future exclusion."""
from datetime import date, datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('comparison', ROOT / 'deploy/chain/comparison.py')
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


BINARY = ROOT / 'build/research/src/backtests/algotrading_research'
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


def add_unranked_coin(database, pair):
    """A coin with the same 100 days of history that is absent from the day's ranking."""
    with sqlite3.connect(database) as db:
        rows = db.execute("SELECT date,open,high,low,close,volume,quote_volume FROM ohlcv_data "
                          "WHERE pair='BTCUSDT' AND date<=20261009").fetchall()
        db.executemany('INSERT INTO ohlcv_data VALUES(?,?,?,?,?,?,?,?)', [(pair, *row) for row in rows])


def run_baseline(output, portfolio=None, start=None, open_prices=None):
    manifest = {'strategy_config': str(comparison.PROFILE), 'portfolio_config': str(portfolio or comparison.PORTFOLIO),
                'initial_cash': 100000, 'commission_rate': .001, 'warmup_days': 100,
                'cycles': [{'database': str(output / 'market.sqlite'), 'decision_date': 20261009,
                            'execution_date': 20261010, 'open_prices': open_prices or {'BTCUSDT': 400}}],
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

    def test_held_coin_outside_the_top_50_is_priced_and_resized(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            add_unranked_coin(market_database(output, wiggle=True), 'ETHUSDT')
            # ETH is held with an active signal but has left the ranking (top 50).
            start = {'cash': 98000, 'positions': {'ETHUSDT': 10},
                     'strategies': [{'strategy_id': 1, 'signals': {'ETHUSDT': 1}, 'desired_weights': {'ETHUSDT': 0.02},
                                     'virtual_positions': {'ETHUSDT': 10}}]}
            row = run_baseline(output, VOL_TARGET, start, {'BTCUSDT': 400, 'ETHUSDT': 400})['rows'][0]
            # Its close(T) still prices the 20% relative rule (risk service window), so the
            # small 2% position is resized to the volatility target like a ranked coin.
            self.assertEqual(row['signals']['ETHUSDT'], 1)
            self.assertGreater(row['positions']['ETHUSDT'], 10)
            # Signals still come from the ranked universe only: BTC enters as before.
            self.assertEqual(row['signals']['BTCUSDT'], 1)

    def test_configuration_change_archives_and_seeds_a_new_study(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            live = {'account_cash': 1000, 'account_positions': {'BTCUSDT': 2},
                    'strategies': [{'strategy_id': 1, 'signals': {}, 'desired_weights': {},
                                    'virtual_positions': {'BTCUSDT': 2}}]}
            old = {'fingerprint': 'a' * 64, 'cycles': [{'decision_date': 20261012, 'execution_date': 20261013, 'live': live}]}
            comparison.write_json(output / 'study.json', old)
            comparison.write_json(output / 'comparison.json', {'rows': [{'date': '2026-10-13', 'liveEquity': 1}]})
            (output / 'market-20261012.sqlite').write_text('x')
            intent = {'timestamp': 20261012, 'strategies': [{'strategy_id': 1, 'signals': {'BTCUSDT': 1}}]}
            with self.assertRaises(ValueError):  # the account moved on after the last capture
                comparison.restart_study(output, old, 'b' * 64, {**live, 'account_cash': 999}, intent)
            with self.assertRaises(ValueError):
                comparison.restart_study(output, old, 'b' * 64, live, {**intent, 'timestamp': 20261013})
            self.assertFalse((output / ('archive-' + 'a' * 12)).exists())
            study = comparison.restart_study(output, old, 'b' * 64, live, intent)
            archive = output / ('archive-' + 'a' * 12)
            self.assertTrue((archive / 'study.json').exists() and (archive / 'market-20261012.sqlite').exists())
            self.assertFalse((output / 'comparison.json').exists())
            self.assertEqual(study['seed'], {'decision_date': 20261012, 'execution_date': 20261013, 'live': live, 'intent': intent})
            self.assertEqual(study['previousRows'], [{'date': '2026-10-13', 'liveEquity': 1}])
            self.assertEqual(json.loads((output / 'study.json').read_text()), study)
            self.assertEqual(comparison.baseline_start(study['seed'])['strategies'][0]['signals'], {'BTCUSDT': 1})

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


class KrakenLiveComparisonTest(unittest.TestCase):
    def test_baseline_sizes_with_collateral_and_takes_capital_flows(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            market_database(output)
            def run(extra):
                cycle = {'database': str(output / 'market.sqlite'), 'decision_date': 20261009,
                         'execution_date': 20261010, 'open_prices': {'BTCUSDT': 400}, **extra}
                comparison.write_json(output / 'manifest.json', {
                    'strategy_config': str(comparison.PROFILE), 'portfolio_config': str(EQUAL_WEIGHT),
                    'initial_cash': 100000, 'commission_rate': .001, 'warmup_days': 100,
                    'cycles': [cycle], 'output': str(output / 'result.json')})
                subprocess.run([str(BINARY)], check=True, capture_output=True,
                               env=dict(os.environ, ALGOTRADING_PAPER_BASELINE_MANIFEST=str(output / 'manifest.json')))
                return json.loads((output / 'result.json').read_text())['rows'][0]
            # 10% of (100000 cash + 100000 BTC collateral) at 400: 50 units, fee 20.
            row = run({'collateral_usd': 100000})
            self.assertAlmostEqual(row['positions']['BTCUSDT'], 50)
            self.assertAlmostEqual(row['fees'], 20)
            self.assertAlmostEqual(row['cash'], 100000 - 20000 - 20)
            # A 1000 USD deposit is in the expected cash and capital too (10% of 101000).
            row = run({'capital_flow_usd': 1000})
            self.assertAlmostEqual(row['positions']['BTCUSDT'], 25.25)
            self.assertAlmostEqual(row['cash'], 101000 - 10100 - 10.1)

    def test_difference_parts_since_the_start(self):
        live = {'account_cash': 880, 'account_positions': {'BTCUSDT': 1}}
        cycles = [{'decision_date': 20261012, 'execution_date': 20261013, 'open_prices': {'BTCUSDT': 100},
                   'signals': {'BTCUSDT': 1}, 'live': live, 'skipped': ['PEPEUSDT'],
                   'plan': {'submit_orders': [{'order_id': 7, 'coin': 'BTCUSDT'}]}}]
        baseline = {'rows': [{'decisionDate': 20261012, 'executionDate': 20261013, 'cash': 899.9, 'equity': 999.9,
                              'fees': 0.1, 'positions': {'BTCUSDT': 1}, 'signals': {'BTCUSDT': 1}}]}
        fills = [{'fill_id': 1, 'order_id': 7, 'timestamp': 20261013, 'coin': 'BTCUSDT', 'side': 0,
                  'quantity': 1, 'price': 100.5, 'commission': 0},
                 {'fill_id': 2, 'order_id': 99, 'timestamp': 20261013, 'coin': 'BTCUSDT', 'side': 1,
                  'quantity': 0.1, 'price': 90, 'commission': 0}]   # manual order: not slippage
        costs = [{'cost_id': 'a', 'kind': 'FEE', 'timestamp': 20261013, 'amount': -0.05},
                 {'cost_id': 'b', 'kind': 'FUNDING', 'timestamp': 20261013, 'amount': -0.2},
                 {'cost_id': 'c', 'kind': 'INTEREST', 'timestamp': 20261013, 'amount': -0.01},
                 {'cost_id': 'd', 'kind': 'CAPITAL_FLOW', 'timestamp': 20261013, 'amount': 50},
                 {'cost_id': 'e', 'kind': 'FEE', 'timestamp': 20261012, 'amount': -9}]   # before the start
        seed = {'decision_date': 20261011, 'execution_date': 20261012}
        rows, _ = comparison.compare_live(cycles, baseline, seed, fills, costs)
        row = rows[0]
        self.assertAlmostEqual(row['liveFees'], 0.05)
        self.assertAlmostEqual(row['expectedFees'], 0.1)
        self.assertAlmostEqual(row['liveFunding'], -0.2)
        self.assertAlmostEqual(row['liveInterest'], -0.01)
        self.assertAlmostEqual(row['capitalFlows'], 50)
        self.assertAlmostEqual(row['slippage'], 0.5)
        self.assertEqual(row['skipped'], ['PEPEUSDT'])

    def test_binance_open_of_the_execution_day(self):
        requested = []
        class Response:
            def __init__(self, body): self.body = body
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def read(self, limit): return self.body
        start = int(datetime(2026, 10, 14, tzinfo=timezone.utc).timestamp() * 1000)
        def urlopen(url, timeout):
            requested.append(url)
            open_time = start if 'BTCUSDT' in url else start - 86400000
            return Response(json.dumps([[open_time, '62000.5', '1', '1', '1', '1']]).encode())
        original = comparison.urllib.request.urlopen
        comparison.urllib.request.urlopen = urlopen
        try:
            self.assertEqual(comparison.binance_opens(20261014, {'BTCUSDT'}), {'BTCUSDT': 62000.5})
            self.assertIn('startTime=' + str(start), requested[0])
            with self.assertRaises(ValueError):   # a candle of another day is never used
                comparison.binance_opens(20261014, {'ETHUSDT'})
        finally:
            comparison.urllib.request.urlopen = original

    def test_live_study_seeds_then_compares_and_follows_late_fees(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            strategies = [{'strategy_id': 1, 'signals': {}, 'desired_weights': {}, 'virtual_positions': {}}]
            def observed(decision, cash, costs):
                live = {'account_cash': cash, 'account_positions': {'BTCUSDT': 1}, 'strategies': strategies,
                        'orders': [{'order_id': 7, 'status': 4, 'active_from': decision + 1, 'coin': 'BTCUSDT'},
                                   {'order_id': 8, 'status': 6, 'active_from': decision + 1, 'coin': 'PEPEUSDT',
                                    'last_message': 'MARKET_UNAVAILABLE: PEPE is not in the symbol map'}],
                        'last_bar_close_timestamp': decision, 'last_execution_timestamp': decision + 1}
                return {'snapshot': live, 'plan': {'decision_timestamp': decision, 'submit_orders': []},
                        'intent': {'timestamp': decision, 'strategies': [{'strategy_id': 1, 'signals': {'BTCUSDT': 1}}]},
                        'strategyIdentity': comparison.PROFILE.read_text(), 'riskIdentity': comparison.PORTFOLIO.read_text(),
                        'fills': [], 'costs': costs, 'collateral': {str(decision): 613.8}}
            state = {'observed': observed(20261012, 900, [])}
            manifests = []
            def fake_baseline(out, manifest, binary, image, count):
                manifests.append(manifest)
                return {'rows': [{'decisionDate': c['decision_date'], 'executionDate': c['execution_date'], 'cash': 900,
                                  'equity': 1000, 'fees': 0.1, 'positions': {'BTCUSDT': 1}, 'signals': {'BTCUSDT': 1}}
                                 for c in manifest['cycles']]}
            stubs = {'query_live': lambda container: state['observed'],
                     'freeze_market': lambda container, out, decision: out / 'market.sqlite',
                     'binance_opens': lambda day, symbols: {symbol: 100.0 for symbol in symbols},
                     'run_baseline': fake_baseline}
            originals = {name: getattr(comparison, name) for name in stubs}
            for name, stub in stubs.items():
                setattr(comparison, name, stub)
            try:
                comparison.collect_live(('postgres', 'market-data'), output, Path('/nonexistent'))
                study = json.loads((output / 'study.json').read_text())
                self.assertEqual(study['seed']['decision_date'], 20261012)
                self.assertEqual(json.loads((output / 'comparison.json').read_text())['rows'], [])

                flow = {'cost_id': 'f', 'kind': 'CAPITAL_FLOW', 'timestamp': 20261014, 'amount': 50}
                state['observed'] = observed(20261013, 950, [flow])
                comparison.collect_live(('postgres', 'market-data'), output, Path('/nonexistent'))
                cycle = manifests[-1]['cycles'][0]
                self.assertEqual((cycle['collateral_usd'], cycle['capital_flow_usd'], cycle['skipped']),
                                 (613.8, 50, ['PEPEUSDT']))
                self.assertEqual(manifests[-1]['start']['cash'], 900)
                self.assertEqual(manifests[-1]['start']['strategies'][0]['signals'], {'BTCUSDT': 1})
                self.assertEqual(strategies[0]['signals'], {})  # execution state never owns these signals
                self.assertEqual(manifests[-1]['commission_rate'], 0.0005)

                # The fee arrives minutes later: same cycle, the row follows the account.
                fee = {'cost_id': 'g', 'kind': 'FEE', 'timestamp': 20261014, 'amount': -0.04}
                state['observed'] = observed(20261013, 949.96, [flow, fee])
                comparison.collect_live(('postgres', 'market-data'), output, Path('/nonexistent'))
                row = json.loads((output / 'comparison.json').read_text())['rows'][-1]
                self.assertAlmostEqual(row['liveCash'], 949.96)
                self.assertAlmostEqual(row['liveFees'], 0.04)
                self.assertEqual(len(json.loads((output / 'study.json').read_text())['cycles']), 1)
            finally:
                for name, original in originals.items():
                    setattr(comparison, name, original)


class LiveRestingStopTest(unittest.TestCase):
    def test_resting_stop_loss_does_not_block_the_live_study(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            strategies = [{'strategy_id': 1, 'signals': {}, 'desired_weights': {}, 'virtual_positions': {}}]
            stop = {'order_id': 9, 'status': 2, 'active_from': 20261013, 'coin': 'BTCUSDT', 'side': 1,
                    'last_message': 'accepted by venue', 'position_stop_price': 70.0}
            market = {'order_id': 10, 'status': 2, 'active_from': 20261013, 'coin': 'BTCUSDT', 'side': 0,
                      'last_message': 'accepted by venue'}
            def observed(orders):
                live = {'account_cash': 900, 'account_positions': {'BTCUSDT': 1}, 'strategies': strategies,
                        'orders': orders, 'last_bar_close_timestamp': 20261012, 'last_execution_timestamp': 20261013}
                return {'snapshot': live, 'plan': {'decision_timestamp': 20261012, 'submit_orders': []},
                        'intent': {'timestamp': 20261012, 'strategies': [{'strategy_id': 1, 'signals': {'BTCUSDT': 1}}]},
                        'strategyIdentity': comparison.PROFILE.read_text(), 'riskIdentity': comparison.PORTFOLIO.read_text(),
                        'fills': [], 'costs': [], 'collateral': {}}
            state = {'observed': observed([stop, market])}
            with patch.object(comparison, 'query_live', lambda container: state['observed']):
                with self.assertRaisesRegex(ValueError, 'terminal Kraken order lifecycle'):
                    comparison.collect_live(('postgres', 'market-data'), output, Path('/nonexistent'))
                self.assertFalse((output / 'study.json').exists())

                state['observed'] = observed([stop])
                comparison.collect_live(('postgres', 'market-data'), output, Path('/nonexistent'))
                self.assertEqual(json.loads((output / 'study.json').read_text())['seed']['decision_date'], 20261012)
                self.assertEqual(json.loads((output / 'comparison.json').read_text())['status'], 'OBSERVING')


class BaselineSeedRegressionTest(unittest.TestCase):
    def seed(self):
        return {'decision_date': 20261008, 'execution_date': 20261009,
                'live': {'account_cash': 50000, 'account_positions': {'BTCUSDT': 10},
                         'strategies': [{'strategy_id': 1, 'signals': {}, 'desired_weights': {},
                                         'virtual_positions': {'BTCUSDT': 10}}]}}

    def intent(self, timestamp=20261008):
        return {'timestamp': timestamp, 'strategies': [{'strategy_id': 1, 'signals': {'BTCUSDT': 1}}]}

    def test_historical_seed_repair_preserves_results_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            study = {'seed': self.seed(), 'cycles': []}
            comparison.write_json(output / 'study.json', study)
            (output / 'fast.json').write_text('original baseline')
            (output / 'comparison.json').write_text('original comparison')
            with patch.object(comparison, 'docker', return_value=json.dumps(self.intent())) as query:
                comparison.restore_seed_intent(output, study, 'postgres', self.intent(20261009))
                self.assertIn('timestamp = 20261008', query.call_args.args[-1])
                first = (output / 'study.json').read_bytes()
                comparison.restore_seed_intent(output, study, 'postgres', self.intent(20261010))
                query.assert_called_once()
            self.assertEqual((output / 'study.json').read_bytes(), first)
            self.assertEqual((output / 'before-seed-repair/fast.json').read_text(), 'original baseline')
            self.assertEqual((output / 'before-seed-repair/comparison.json').read_text(), 'original comparison')
            self.assertNotIn('intent', json.loads((output / 'before-seed-repair/study.json').read_text())['seed'])
            self.assertEqual(comparison.baseline_start(study['seed'])['strategies'][0]['signals'], {'BTCUSDT': 1})
            self.assertEqual(study['seed']['live']['strategies'][0]['signals'], {})

    def test_missing_wrong_date_or_wrong_strategy_never_repairs_the_journal(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            for payload in ('', json.dumps(self.intent(20261009)),
                            json.dumps({'timestamp': 20261008, 'strategies': [{'strategy_id': 9, 'signals': {}}]})):
                study = {'seed': self.seed(), 'cycles': []}
                comparison.write_json(output / 'study.json', study)
                before = (output / 'study.json').read_bytes()
                with patch.object(comparison, 'docker', return_value=payload), self.assertRaises(ValueError):
                    comparison.restore_seed_intent(output, study, 'postgres', self.intent(20261010))
                self.assertEqual((output / 'study.json').read_bytes(), before)
                self.assertFalse((output / 'before-seed-repair').exists())

    def test_signal_between_entry_and_exit_is_held_after_seed_repair(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            database = market_database(output)
            # 95 rising bars followed by five falls gives RSI(7) ~46.27: keep an
            # existing signal, but do not enter a new one. The future bar stays poisoned.
            with sqlite3.connect(database) as db:
                dates = db.execute('SELECT date FROM ohlcv_data WHERE date<=20261009 ORDER BY date').fetchall()
                for index, (date_value,) in enumerate(dates):
                    price = 100 + index if index <= 94 else 194 - (index - 94)
                    db.execute('UPDATE ohlcv_data SET open=?,high=?,low=?,close=? WHERE date=?',
                               (price, price, price, price, date_value))
            seed = self.seed()
            original = {'cash': seed['live']['account_cash'], 'positions': seed['live']['account_positions'],
                        'strategies': seed['live']['strategies']}
            seed['intent'] = self.intent()
            before = run_baseline(output, EQUAL_WEIGHT, original)['rows'][0]
            after = run_baseline(output, EQUAL_WEIGHT, comparison.baseline_start(seed))['rows'][0]
            self.assertEqual(before['signals'].get('BTCUSDT', 0), 0)
            self.assertAlmostEqual(before['positions'].get('BTCUSDT', 0), 0)
            self.assertEqual(after['signals']['BTCUSDT'], 1)
            self.assertAlmostEqual(after['positions']['BTCUSDT'], 10)
            self.assertAlmostEqual(after['cash'], 50000)

    def test_cache_recomputes_same_length_when_seed_changes_and_retries_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            seed = self.seed()
            manifest = {'strategy_config': str(comparison.PROFILE), 'portfolio_config': str(EQUAL_WEIGHT),
                        'cycles': [{'decision_date': 20261009, 'execution_date': 20261010,
                                    'database': str(output / 'market.sqlite'), 'open_prices': {'BTCUSDT': 400}}],
                        'start': {'cash': 50000, 'positions': {}, 'strategies': seed['live']['strategies']},
                        'output': str(output / 'fast.json')}
            def execute(*args, **kwargs):
                comparison.write_json(output / 'fast.json', {'rows': [{'signals': manifest['start']['strategies'][0]['signals']}]})
            with patch.object(comparison.subprocess, 'run', side_effect=execute) as run:
                comparison.run_baseline(output, manifest, Path('/runner'), None, 1)
                comparison.run_baseline(output, manifest, Path('/runner'), None, 1)
                self.assertEqual(run.call_count, 1)
                manifest['start']['strategies'][0]['signals'] = {'BTCUSDT': 1}
                result = comparison.run_baseline(output, manifest, Path('/runner'), None, 1)
                self.assertEqual(run.call_count, 2)
                self.assertEqual(result['rows'][0]['signals'], {'BTCUSDT': 1})
                manifest['start']['cash'] = 51000
                saved_identity = (output / 'fast-input.sha256').read_bytes()
                run.side_effect = subprocess.CalledProcessError(1, '/runner')
                with self.assertRaises(subprocess.CalledProcessError):
                    comparison.run_baseline(output, manifest, Path('/runner'), None, 1)
                self.assertEqual((output / 'fast-input.sha256').read_bytes(), saved_identity)
                run.side_effect = execute
                comparison.run_baseline(output, manifest, Path('/runner'), None, 1)
                self.assertEqual(run.call_count, 4)


if __name__ == '__main__':
    unittest.main()
