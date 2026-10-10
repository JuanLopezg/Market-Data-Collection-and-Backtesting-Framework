#!/usr/bin/env python3
"""Bounded Kraken public-reader, durable observations and BTC/USD scenario checks."""
import argparse
import copy
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'live_trading'))
import kraken_shadow as kraken


def catalogue(now):
    clock = datetime.fromtimestamp(now, timezone.utc).isoformat()
    instrument = {'symbol': 'PF_XBTUSD', 'type': 'flexible_futures', 'base': 'XBT', 'quote': 'USD',
        'tradeable': True, 'isExpired': False, 'tradfi': False, 'contractSize': 1,
        'contractValueTradePrecision': 4, 'tickSize': 1, 'postOnly': False}
    ticker = {'symbol': 'PF_XBTUSD', 'tag': 'perpetual', 'suspended': False,
        'markPrice': '50000', 'indexPrice': '49000', 'bid': '49999', 'ask': '50001', 'postOnly': False}
    return ({'result': 'success', 'serverTime': clock, 'instruments': [instrument]},
            {'result': 'success', 'serverTime': clock, 'tickers': [ticker]})


def plan(now, coin='BTCUSDT'):
    day = int((datetime.fromtimestamp(now, timezone.utc).date() - timedelta(days=1)).strftime('%Y%m%d'))
    return {'metadata': {'schema_version': 1, 'message_id': 'test-plan'},
        'decision_timestamp': day, 'state_revision': 1, 'decisions': {'decision_timestamp': day},
        'reference_closes': {'date': day, 'closes': {coin: 199}}, 'cancel_order_ids': [],
        'submit_orders': [{'economic_order_id': 'test-order', 'coin': coin, 'side': 0,
            'decision_timestamp': day, 'state_revision': 1, 'reference_close': 199,
            'notional_usd': 1000, 'delta_notional_usd': 1000}]}


def scenario(**changes):
    return {'scope': 'HYPOTHETICAL_CROSS_MARGIN_SCENARIO', 'profit_currency': 'USD',
        'btc_balance': '1', 'usd_balance': '5000', 'unrealized_pnl_usd': '-1000',
        'unrealized_funding_usd': '-10', 'initial_margin_usd': '4000', 'maintenance_margin_usd': '2000',
        'cash_debits_usd': '100', 'minimum_usd_reserve': '2000', **changes}


class Reader:
    def __init__(self, snapshot):
        self.value = snapshot
        self.calls = 0
    def snapshot(self):
        self.calls += 1
        return self.value


class KrakenTest(unittest.TestCase):
    def setUp(self):
        self.now = time.time()
        self.instruments, self.tickers = catalogue(self.now)
        self.snapshot = kraken.normalize(self.instruments, self.tickers, self.now, 'live')

    def test_mark_quantity_preserves_source_plan_and_uses_index_for_collateral(self):
        original = plan(self.now)
        before = copy.deepcopy(original)
        result = kraken.assess_kraken(original, self.snapshot, self.now)
        row = result['orders'][0]
        self.assertEqual(row['hypothetical_quantity_at_current_mark'], '0.02')
        self.assertEqual(row['source_reference_close'], 199)
        self.assertEqual(row['kraken_symbol'], 'PF_XBTUSD')
        self.assertEqual(original, before)
        self.assertFalse(result['submitted'])
        self.assertIsNone(result['fills'])
        self.assertEqual(self.snapshot['btc_index_price_usd'], '49000')

    def test_missing_market_skips_but_failed_catalogue_is_unavailable(self):
        result = kraken.assess_kraken(plan(self.now, 'NOTLISTEDUSDT'), self.snapshot, self.now)
        self.assertEqual(result['orders'][0]['reason'], 'MULTI_M_PERPETUAL_UNAVAILABLE')
        with self.assertRaises(ValueError):
            kraken.normalize(self.instruments, {**self.tickers, 'tickers': []}, self.now, 'live')
        with self.assertRaises(ValueError):
            kraken.normalize(self.instruments, {**self.tickers, 'result': 'error'}, self.now, 'live')

    def test_expired_inverse_and_suspended_never_become_active_multi_m(self):
        for changes in ({'isExpired': True}, {'tradeable': False}):
            instruments = copy.deepcopy(self.instruments)
            instruments['instruments'].append({**instruments['instruments'][0], 'symbol': 'PF_ETHUSD', 'base': 'ETH', **changes})
            normalized = kraken.normalize(instruments, self.tickers, self.now, 'live')
            self.assertFalse(normalized['markets']['ETH']['trading'])
        instruments = copy.deepcopy(self.instruments)
        instruments['instruments'].append({**instruments['instruments'][0], 'symbol': 'PI_XBTUSD', 'type': 'futures_inverse'})
        self.assertEqual(len(kraken.normalize(instruments, self.tickers, self.now, 'live')['markets']), 1)
        self.tickers['tickers'][0]['suspended'] = True
        with self.assertRaises(ValueError):
            kraken.normalize(self.instruments, self.tickers, self.now, 'live')

    def test_duplicate_metadata_stale_server_and_invalid_contract_rules_rejected(self):
        for changes in ({'contractSize': 1000}, {'contractValueTradePrecision': True}, {'quote': 'EUR'}, {'postOnly': 'false'}):
            instruments = copy.deepcopy(self.instruments)
            instruments['instruments'][0].update(changes)
            with self.assertRaises(ValueError):
                kraken.normalize(instruments, self.tickers, self.now, 'live')
        self.instruments['instruments'].append(copy.deepcopy(self.instruments['instruments'][0]))
        with self.assertRaises(ValueError):
            kraken.normalize(self.instruments, self.tickers, self.now, 'live')
        for offset in (-60, 60):
            instruments, tickers = catalogue(self.now + offset)
            with self.assertRaises(ValueError):
                kraken.normalize(instruments, tickers, self.now, 'live')

    def test_empty_book_is_retryable_unavailability_not_unsupported(self):
        del self.tickers['tickers'][0]['ask']
        snapshot = kraken.normalize(self.instruments, self.tickers, self.now, 'live')
        self.assertTrue(snapshot['markets']['BTC']['trading'])
        with self.assertRaises(ValueError):
            kraken.assess_kraken(plan(self.now), snapshot, self.now)

    def test_scaled_source_maps_underlying_without_reusing_source_contract_quantity(self):
        instruments = copy.deepcopy(self.instruments)
        tickers = copy.deepcopy(self.tickers)
        instruments['instruments'].append({**instruments['instruments'][0], 'symbol': 'PF_PEPEUSD', 'base': 'PEPE',
                                           'contractValueTradePrecision': -3})
        tickers['tickers'].append({**tickers['tickers'][0], 'symbol': 'PF_PEPEUSD', 'markPrice': '.000001',
                                  'indexPrice': '.000001', 'bid': '.0000009', 'ask': '.0000011'})
        snapshot = kraken.normalize(instruments, tickers, self.now, 'live')
        row = kraken.assess_kraken(plan(self.now, '1000PEPEUSDT'), snapshot, self.now)['orders'][0]
        self.assertEqual(Decimal(row['hypothetical_quantity_at_current_mark']), Decimal('1000000000'))
        self.assertEqual(row['quantity_step'], '1000')
        self.assertEqual(kraken.source_asset('1INCHUSDT'), '1INCH')

    def test_btc_is_discounted_collateral_not_spendable_usd_or_a_fee(self):
        input_value = scenario()
        before = copy.deepcopy(input_value)
        result = kraken.collateral_scenario(input_value, self.snapshot, self.now)
        self.assertEqual(result['btc_market_value_usd'], '49000')
        self.assertEqual(result['btc_collateral_after_haircut_usd'], '48510.00')
        self.assertEqual(result['estimated_margin_equity_usd'], '52400.00')
        self.assertEqual(result['usd_reserve_after_unrealized_losses'], '3890')
        self.assertEqual(result['state'], 'SCENARIO_CHECKS_PASS')
        self.assertIsNone(result['can_trade'])
        self.assertFalse(result['btc_sold'])
        self.assertEqual(input_value, before)

    def test_usd_exhaustion_flags_conversion_and_uncovered_loss_despite_btc(self):
        result = kraken.collateral_scenario(scenario(usd_balance='50'), self.snapshot, self.now)
        self.assertIn('USD_DEBITS_REQUIRE_CONVERSION', result['reasons'])
        self.assertIn('UNREALIZED_LOSS_NOT_BACKED_BY_USD', result['reasons'])
        self.assertIn('USD_RESERVE_LOW', result['reasons'])
        self.assertGreater(Decimal(result['estimated_margin_equity_usd']), 4000)
        self.assertFalse(result['btc_sold'])

    def test_btc_fall_and_pnl_funding_signs_can_breach_margin(self):
        snapshot = {**self.snapshot, 'btc_index_price_usd': '1000'}
        result = kraken.collateral_scenario(scenario(), snapshot, self.now)
        self.assertEqual(result['estimated_margin_equity_usd'], '4880.00')
        result = kraken.collateral_scenario(scenario(usd_balance='0'), snapshot, self.now)
        self.assertIn('INITIAL_MARGIN_SHORTFALL', result['reasons'])
        self.assertIn('MAINTENANCE_MARGIN_BREACH', result['reasons'])
        result = kraken.collateral_scenario(scenario(unrealized_pnl_usd='1000', unrealized_funding_usd='10'), snapshot, self.now)
        self.assertEqual(result['estimated_margin_equity_usd'], '6900.00')

    def test_wrong_wallet_currency_nonfinite_and_missing_margin_fail(self):
        for changes in ({'profit_currency': 'BTC'}, {'scope': 'REAL_ACCOUNT'}, {'btc_balance': '-1'},
                        {'usd_balance': 'NaN'}, {'maintenance_margin_usd': '5000'}):
            with self.assertRaises(ValueError):
                kraken.collateral_scenario(scenario(**changes), self.snapshot, self.now)
        value = scenario()
        del value['initial_margin_usd']
        with self.assertRaises(KeyError):
            kraken.collateral_scenario(value, self.snapshot, self.now)

    def test_journal_resume_conflict_source_identity_and_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            reader = Reader(self.snapshot)
            def open_journal(environment='live'):
                return kraken.ShadowJournal(directory, 'a' * 64, kraken.assess_kraken, kraken.SCOPE,
                                            {'environment': environment, 'schema': 1})
            journal = open_journal()
            original = plan(self.now)
            result = journal.observe(original, reader, self.now)
            journal.close()
            journal = open_journal()
            self.assertEqual(journal.observe(original, reader, self.now), result)
            self.assertEqual(reader.calls, 1)
            conflict = copy.deepcopy(original)
            conflict['cancel_order_ids'] = [1]
            with self.assertRaises(ValueError):
                journal.observe(conflict, reader, self.now)
            retry = copy.deepcopy(original)
            retry['metadata']['message_id'] = 'retry'
            reader.value = {**self.snapshot, 'complete': False}
            self.assertEqual(journal.observe(retry, reader, self.now)['state'], 'UNAVAILABLE')
            reader.value = self.snapshot
            self.assertEqual(journal.observe(retry, reader, self.now)['state'], 'OBSERVED')
            journal.close()
            with self.assertRaises(ValueError):
                open_journal('demo')

    def test_fixture_journal_cannot_be_relabelled_as_kraken(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = kraken.ShadowJournal(directory, 'a' * 64)
            journal.close()
            with self.assertRaises(ValueError):
                kraken.ShadowJournal(directory, 'a' * 64, kraken.assess_kraken, kraken.SCOPE, {'environment': 'live'})

    def test_reader_has_two_fixed_public_gets_and_no_redirects(self):
        class Response:
            headers = {'Date': 'fixture'}
            def __init__(self, body): self.body = body
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self, bound): return self.body
        with tempfile.TemporaryDirectory() as directory:
            reader = kraken.KrakenPublicReader('demo', directory)
            with patch.object(reader.client, 'open', side_effect=[Response(json.dumps(self.instruments).encode()),
                                                                  Response(json.dumps(self.tickers).encode())]) as spy:
                reader.snapshot()
                self.assertEqual([call.args[0].get_method() for call in spy.call_args_list], ['GET', 'GET'])
                self.assertEqual([call.args[0].full_url for call in spy.call_args_list],
                    ['https://demo-futures.kraken.com/derivatives/api/v3/instruments',
                     'https://demo-futures.kraken.com/derivatives/api/v3/tickers'])
                self.assertTrue(all(not call.args[0].data for call in spy.call_args_list))
            with self.assertRaises(ValueError): reader.read('sendorder')
            with self.assertRaises(ValueError):
                kraken.NoRedirect().redirect_request(None, None, 302, None, None, 'https://other.example')


def audit(directory):
    report = json.loads((directory / 'report.json').read_text(encoding='utf-8'))
    capture = Path(report['raw_capture'])
    # Windows-written evidence is audited through the supplied WSL study path.
    if not capture.exists():
        capture = directory / 'raw' / str(capture).replace('\\', '/').rsplit('/', 1)[-1]
    raw = []
    for name in ('instruments', 'tickers'):
        body = (capture / (name + '.json')).read_bytes()
        receipt = json.loads((capture / (name + '-receipt.json')).read_text())
        assert receipt['method'] == 'GET' and receipt['url'] == kraken.HOSTS[report['environment']] + '/derivatives/api/v3/' + name
        assert hashlib.sha256(body).hexdigest() == receipt['sha256']
        raw.append(json.loads(body, parse_float=Decimal))
    snapshot = kraken.normalize(*raw, report['observed_at'], report['environment'])
    assert kraken.digest(snapshot) == report['snapshot_hash']
    assert len(snapshot['markets']) == report['market_count'] and report['submitted'] is False
    for result in report['plans']:
        assert result['submitted'] is False and result['scope'] == kraken.SCOPE
        matching_capture = None
        for retained in (directory / 'raw').glob('*/snapshot.json'):
            retained_snapshot = json.loads(retained.read_text(encoding='utf-8'))
            if kraken.digest(retained_snapshot) == result['venue_snapshot_hash']:
                matching_capture = retained.parent
                break
        assert matching_capture is not None
        inputs = json.loads((matching_capture / 'producer-plans.json').read_text(encoding='utf-8'))
        original = next(value for value in inputs if value['metadata']['message_id'] == result['message_id'])
        assert kraken.digest(original) == result['plan_hash']
        observed = kraken.assess_kraken(original, retained_snapshot, retained_snapshot['observed_at'])
        assert all(result[key] == value for key, value in observed.items())
        for row in result['orders']:
            if row['state'] == 'OBSERVED':
                mark = Decimal(row['observed_mark_usd'])
                assert Decimal(row['observed_notional_usd']) <= Decimal(str(row['notional_usd']))
                assert Decimal(row['observed_notional_usd']) == Decimal(row['hypothetical_quantity_at_current_mark']) * mark
    if report['collateral_scenario']:
        assert kraken.collateral_scenario(report['collateral_scenario']['input'], snapshot, report['observed_at']) == report['collateral_scenario']
    print('KRAKEN-AUDIT: PASS: raw hashes, fixed GET endpoints, catalogue and read-only observations')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(KrakenTest))
    if not result.wasSuccessful(): raise SystemExit(1)
    if args.snapshot: audit(args.snapshot)
