#!/usr/bin/env python3
"""Offline acceptance of Kraken account reads; no credentials or network required."""
import argparse
import base64
import copy
from contextlib import redirect_stdout
from datetime import datetime, timezone
import hashlib
import hmac
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'live_trading'))
import kraken_account as account

POLICY = {'minimum_usd_reserve': '500', 'assumed_future_cash_debits_usd': '50'}
RELATIVE_POLICY = {'minimum_account_equity_usd': '50', 'minimum_usd_reserve_fraction': '0.10',
                   'assumed_future_cash_debits_usd': '0'}
TEST_KEY = 'synthetic-test-key'
TEST_SECRET = base64.b64encode(b'local-test-secret-never-a-real-key').decode()


def fixture(now):
    clock = datetime.fromtimestamp(now, timezone.utc).isoformat()
    success = {'result': 'success', 'serverTime': clock}
    wallet = {'type': 'multiCollateralMarginAccount', 'currencies': {
        'XBT': {'quantity': '0.1', 'value': '5000', 'collateral': '4950', 'available': '0.1'},
        'USD': {'quantity': '2000', 'value': '2000', 'collateral': '2000', 'available': '2000'}},
        'collateralValue': '6950', 'marginEquity': '6849', 'availableMargin': '6049',
        'initialMargin': '500', 'initialMarginWithOrders': '800', 'maintenanceMargin': '250',
        'pnl': '-100', 'unrealizedFunding': '-1', 'totalUnrealized': '-101',
        'totalUnrealizedAsMargin': '-101'}
    return {'scope': account.FIXTURE_SCOPE, 'environment': 'live', 'observed_at': now,
        'permission_checked_at': now, 'permissions': {'general': 'READ_ONLY', 'transfer': 'NO_ACCESS',
        'account_identity_hash': 'a' * 64}, 'wallets': {**success, 'accounts': {'flex': wallet}},
        'positions': {**success, 'openPositions': [{'symbol': 'PF_XBTUSD', 'side': 'short',
        'size': '0.01', 'price': '50000', 'unrealizedPnl': '-100', 'unrealizedFunding': '-1',
        'pnlCurrency': 'USD'}]}, 'orders': {**success, 'openOrders': [{
        'order_id': 'synthetic-partial-order', 'symbol': 'PF_XBTUSD', 'side': 'buy',
        'filledSize': '0.002', 'unfilledSize': '0.003', 'reduceOnly': True,
        'receivedTime': clock, 'lastUpdateTime': clock}]},
        'preferences': {**success, 'preferences': [{'symbol': 'PF_XBTUSD', 'pnlCurrency': 'USD'}]}}


class Transport:
    def __init__(self, bundle, permissions=None):
        self.bundle = bundle
        self.calls = []
        self.permissions = permissions or {'apiKey': TEST_KEY, 'accountUid': 'synthetic-account',
            'permissions': {'general': 'READ_ONLY', 'transfer': 'NO_ACCESS'}}

    def open(self, request, timeout):
        self.calls.append(request)
        assert timeout == 10
        route = next(name for name, paths in account.ROUTES.items() if request.full_url.endswith(paths[0]))
        value = self.permissions if route == 'permissions' else self.bundle[route]
        return io.BytesIO(json.dumps(value).encode())


class AccountTest(unittest.TestCase):
    def setUp(self):
        self.now = time.time()
        self.bundle = fixture(self.now)

    def normalize(self, policy=POLICY):
        return account.normalize_account(self.bundle, policy, self.now)

    def test_venue_margin_funding_orders_and_short_sign(self):
        result = self.normalize()
        self.assertEqual(result['initial_margin_headroom_with_orders_usd'], '6049')
        self.assertEqual(result['maintenance_margin_headroom_usd'], '6599')
        self.assertEqual(result['usd_reserve']['usd_after_assumed_debits_and_unrealized_loss'], '1849')
        self.assertEqual(result['positions'][0]['signed_quantity'], '-0.01')
        self.assertTrue(result['orders'][0]['partially_filled'])
        self.assertIsNone(result['can_trade'])
        self.assertFalse(result['submitted'])
        self.assertFalse(result['core_account_updated'])
        self.assertEqual(result['state'], 'READ_ONLY_CHECKS_PASS')
        # Account collateral is authoritative, not a fixed public-study haircut.
        self.bundle['wallets']['accounts']['flex']['currencies']['XBT']['collateral'] = '4800'
        self.assertEqual(self.normalize()['currencies']['BTC']['collateral'], '4800')

    def test_empty_orders_and_positions_are_valid(self):
        self.bundle['positions']['openPositions'] = []
        self.bundle['orders']['openOrders'] = []
        self.assertEqual(self.normalize()['positions'], [])

    def test_relative_reserve_uses_net_wallet_equity_not_position_notional(self):
        self.bundle['positions']['openPositions'][0]['size'] = '10'
        result = self.normalize(RELATIVE_POLICY)
        self.assertEqual(result['account_equity_policy']['required_usd_reserve'], '684.90')
        self.assertEqual(result['account_equity_policy']['account_equity_usd'], '6849')
        self.assertEqual(result['state'], 'READ_ONLY_CHECKS_PASS')
        self.assertIsNone(result['can_trade'])

    def test_minimum_50_and_ten_percent_boundaries_are_independent(self):
        wallet = self.bundle['wallets']['accounts']['flex']
        wallet.update(marginEquity='50', initialMargin='0', initialMarginWithOrders='0',
                      maintenanceMargin='0', availableMargin='50', totalUnrealized='0')
        wallet['currencies']['USD'].update(quantity='5', available='5')
        result = self.normalize(RELATIVE_POLICY)
        self.assertEqual(result['state'], 'READ_ONLY_CHECKS_PASS')
        self.assertEqual(result['account_equity_policy']['required_usd_reserve'], '5.00')
        wallet['marginEquity'] = '49.99'
        self.assertIn('ACCOUNT_EQUITY_BELOW_MINIMUM', self.normalize(RELATIVE_POLICY)['reasons'])
        wallet['marginEquity'] = '50'
        wallet['currencies']['USD']['available'] = '4.99'
        self.assertIn('USD_RESERVE_LOW', self.normalize(RELATIVE_POLICY)['reasons'])
        self.assertNotIn('ACCOUNT_EQUITY_BELOW_MINIMUM', self.normalize(RELATIVE_POLICY)['reasons'])

    def test_negative_zero_equity_and_missing_usd_cannot_pass_minimum(self):
        wallet = self.bundle['wallets']['accounts']['flex']
        for equity in ('0', '-1'):
            wallet['marginEquity'] = equity
            result = self.normalize(RELATIVE_POLICY)
            self.assertIn('ACCOUNT_EQUITY_BELOW_MINIMUM', result['reasons'])
            self.assertEqual(result['account_equity_policy']['required_usd_reserve'], '0')
        del wallet['currencies']['USD']
        result = self.normalize(RELATIVE_POLICY)
        self.assertIn('USD_BALANCE_NOT_REPORTED', result['reasons'])
        self.assertIn('ACCOUNT_EQUITY_BELOW_MINIMUM', result['reasons'])

    def test_loss_stress_is_separate_from_current_cash_ratio(self):
        wallet = self.bundle['wallets']['accounts']['flex']
        wallet['currencies']['USD'].update(quantity='684.90', available='684.90')
        result = self.normalize(RELATIVE_POLICY)
        self.assertNotIn('USD_RESERVE_LOW', result['reasons'])
        self.assertIn('USD_RESERVE_LOW_AFTER_LOSS_STRESS', result['reasons'])
        self.assertEqual(result['venue_totals_usd']['marginEquity'], '6849')

    def test_relative_policy_validation_and_journal_binding(self):
        for changes in ({'minimum_usd_reserve_fraction': '1.01'},
                        {'minimum_usd_reserve_fraction': '-0.1'},
                        {'minimum_usd_reserve_fraction': True},
                        {'minimum_account_equity_usd': '-1'},
                        {'minimum_usd_reserve_fraction': 'NaN'}, {'unexpected': '0'}):
            with self.assertRaises(ValueError):
                self.normalize({**RELATIVE_POLICY, **changes})
        missing = dict(RELATIVE_POLICY)
        del missing['minimum_account_equity_usd']
        with self.assertRaises(ValueError):
            self.normalize(missing)
        with tempfile.TemporaryDirectory() as directory:
            journal = account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', POLICY)
            try:
                journal.observe(self.bundle, self.now)
            finally:
                journal.close()
            changed = account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', RELATIVE_POLICY)
            try:
                with self.assertRaises(ValueError):
                    changed.observe(self.bundle, self.now)
            finally:
                changed.close()

    def test_usd_exhaustion_and_margin_breach(self):
        wallet = self.bundle['wallets']['accounts']['flex']
        wallet['currencies']['USD']['quantity'] = '20'
        wallet['marginEquity'] = '200'
        wallet['availableMargin'] = '-600'
        reasons = self.normalize()['reasons']
        for expected in ('USD_RESERVE_LOW', 'USD_DEBITS_REQUIRE_CONVERSION',
                         'UNREALIZED_LOSS_NOT_BACKED_BY_USD', 'INITIAL_MARGIN_SHORTFALL',
                         'MAINTENANCE_MARGIN_BREACH'):
            self.assertIn(expected, reasons)

    def test_missing_usd_or_policy_is_not_success(self):
        self.assertIn('USD_RESERVE_POLICY_NOT_CONFIGURED', self.normalize(None)['reasons'])
        del self.bundle['wallets']['accounts']['flex']['currencies']['USD']
        self.assertIn('USD_BALANCE_NOT_REPORTED', self.normalize()['reasons'])
        self.assertIsNone(self.normalize()['usd_reserve'])

    def test_other_collateral_and_settlement_require_review(self):
        currencies = self.bundle['wallets']['accounts']['flex']['currencies']
        currencies['ETH'] = dict(currencies['XBT'])
        self.bundle['preferences']['preferences'][0]['pnlCurrency'] = 'XBT'
        reasons = self.normalize()['reasons']
        self.assertIn('OTHER_COLLATERAL_REQUIRES_REVIEW', reasons)
        self.assertIn('NON_USD_PROFIT_SETTLEMENT', reasons)
        self.assertIn('POSITION_SETTLEMENT_PREFERENCE_CONFLICT', reasons)

    def test_invalid_schema_numbers_and_duplicate_identities(self):
        mutations = [
            lambda b: b['positions'].update(openPositions={}),
            lambda b: b['orders'].update(openOrders={}),
            lambda b: b['preferences'].update(preferences={}),
            lambda b: b['wallets']['accounts']['flex'].update(type='singleCollateralMarginAccount'),
            lambda b: b['wallets']['accounts']['flex'].update(marginEquity='NaN'),
            lambda b: b['wallets']['accounts']['flex'].update(initialMarginWithOrders='400'),
            lambda b: b['positions']['openPositions'][0].update(size='0'),
            lambda b: b['positions']['openPositions'].append(dict(b['positions']['openPositions'][0])),
            lambda b: b['orders']['openOrders'].append(dict(b['orders']['openOrders'][0])),
            lambda b: b['wallets']['accounts']['flex']['currencies'].update(BTC={}),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                self.bundle = fixture(self.now)
                mutate(self.bundle)
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    self.normalize()

    def test_stale_future_naive_and_inverted_clocks(self):
        for route in ('wallets', 'positions', 'orders', 'preferences'):
            for delta in (-31, 6):
                self.bundle = fixture(self.now)
                self.bundle[route]['serverTime'] = datetime.fromtimestamp(self.now + delta, timezone.utc).isoformat()
                with self.assertRaises(ValueError):
                    self.normalize()
        self.bundle = fixture(self.now)
        self.bundle['wallets']['serverTime'] = '2026-10-10T12:00:00'
        with self.assertRaises(ValueError):
            self.normalize()
        self.bundle = fixture(self.now)
        self.bundle['orders']['openOrders'][0]['receivedTime'] = datetime.fromtimestamp(self.now + 1, timezone.utc).isoformat()
        with self.assertRaises(ValueError):
            self.normalize()

    def test_durable_restart_duplicate_conflict_and_ordering(self):
        with tempfile.TemporaryDirectory() as directory:
            def journal(policy=POLICY):
                return account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', policy)
            first = journal()
            expected = first.observe(self.bundle, self.now)
            first.close()
            resumed = journal()
            try:
                self.assertEqual(resumed.observe(self.bundle, self.now), expected)
                changed = copy.deepcopy(self.bundle)
                changed['wallets']['accounts']['flex']['marginEquity'] = '6000'
                with self.assertRaises(ValueError):
                    resumed.observe(changed, self.now)
                with self.assertRaises(ValueError):
                    resumed.observe(fixture(self.now - 1), self.now)
                changed = copy.deepcopy(self.bundle)
                changed['permissions']['account_identity_hash'] = 'b' * 64
                with self.assertRaises(ValueError):
                    resumed.observe(changed, self.now)
                with self.assertRaises(ValueError):
                    resumed.observe(self.bundle, self.now + 31)
                self.assertEqual(resumed.db.execute('SELECT count(*) FROM samples').fetchone()[0], 1)
            finally:
                resumed.close()
            different = journal({'minimum_usd_reserve': '501', 'assumed_future_cash_debits_usd': '50'})
            try:
                with self.assertRaises(ValueError):
                    different.observe(self.bundle, self.now)
            finally:
                different.close()

    def test_invalid_sample_does_not_persist_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', POLICY)
            try:
                self.bundle['scope'] = account.PRIVATE_SCOPE
                with self.assertRaises(ValueError):
                    journal.observe(self.bundle, self.now)
                self.assertEqual(journal.db.execute('SELECT count(*) FROM context').fetchone()[0], 0)
            finally:
                journal.close()

    def test_new_samples_survive_restart_and_environment_cannot_change(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', POLICY)
            try:
                journal.observe(self.bundle, self.now)
                journal.observe(fixture(self.now + 1), self.now + 1)
                other = fixture(self.now + 2)
                other['environment'] = 'demo'
                with self.assertRaises(ValueError):
                    journal.observe(other, self.now + 2)
                self.assertEqual(journal.db.execute('SELECT count(*) FROM samples').fetchone()[0], 2)
            finally:
                journal.close()

    def test_negative_policy_and_missing_numeric_fields_rejected(self):
        with self.assertRaises(ValueError):
            self.normalize({'minimum_usd_reserve': '-1', 'assumed_future_cash_debits_usd': '0'})
        del self.bundle['wallets']['accounts']['flex']['totalUnrealized']
        with self.assertRaises(KeyError):
            self.normalize()

    def test_five_fixed_gets_signature_and_secret_free_result(self):
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        transport = Transport(self.bundle)
        reader.client = transport
        result = reader.snapshot()
        self.assertEqual(len(transport.calls), 5)
        for request, (path, signing_path) in zip(transport.calls, account.ROUTES.values()):
            expected = base64.b64encode(hmac.new(base64.b64decode(TEST_SECRET),
                hashlib.sha256(signing_path.encode()).digest(), hashlib.sha512).digest()).decode()
            self.assertEqual(request.full_url, account.HOSTS['live'] + path)
            self.assertEqual(request.get_method(), 'GET')
            self.assertIsNone(request.data)
            self.assertIsNone(request.get_header('Nonce'))
            self.assertEqual(request.get_header('Authent'), expected)
            self.assertEqual(request.get_header('User-agent'), 'algoTrading-read-only-account')
        rendered = json.dumps(account.normalize_account(result, POLICY, time.time()))
        for secret in (TEST_KEY, TEST_SECRET, 'synthetic-account'):
            self.assertNotIn(secret, rendered)
        with self.assertRaises(ValueError):
            reader._get('sendorder')

    def test_permissions_checked_before_other_reads(self):
        for permissions in ({'general': 'FULL_ACCESS', 'transfer': 'NO_ACCESS'},
                            {'general': 'READ_ONLY', 'transfer': 'FULL_ACCESS'}):
            reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
            reader.client = Transport(self.bundle, {'apiKey': TEST_KEY, 'accountUid': 'fixture',
                                                   'permissions': permissions})
            with self.assertRaises(ValueError):
                reader.snapshot()
            self.assertEqual(len(reader.client.calls), 1)

    def test_failed_transport_and_invalid_json_are_sanitized(self):
        class FailedTransport:
            def open(self, request, timeout):
                raise OSError(TEST_SECRET + TEST_KEY)
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        reader.client = FailedTransport()
        with self.assertRaises(ValueError) as error:
            reader.snapshot()
        self.assertNotIn(TEST_SECRET, str(error.exception))
        class OversizedTransport:
            def open(self, request, timeout):
                return io.BytesIO(b'x' * (2 * 1024 * 1024 + 1))
        reader.client = OversizedTransport()
        with self.assertRaises(ValueError):
            reader.snapshot()
        class HtmlTransport:
            def open(self, request, timeout):
                return io.BytesIO(b'<html>Unavailable</html>')
        reader.client = HtmlTransport()
        with self.assertRaises(ValueError):
            reader.snapshot()
        redirect = account.NoRedirect()
        with self.assertRaises(ValueError):
            redirect.redirect_request(None, None, 302, 'redirect', {}, 'https://other.example/')

    def test_credential_loader_accepts_plain_quoted_crlf_and_bom(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '.env'
            for contents in (
                f'KRAKEN_API_KEY={TEST_KEY}\nKRAKEN_API_SECRET={TEST_SECRET}\n',
                f'\ufeff# Local synthetic fixture\r\nKRAKEN_API_KEY="{TEST_KEY}"\r\nKRAKEN_API_SECRET=\'{TEST_SECRET}\'\r\n',
            ):
                path.write_text(contents, encoding='utf-8')
                with redirect_stdout(io.StringIO()) as output:
                    reader = account.reader_from_credentials(path)
                self.assertEqual(output.getvalue(), '')
                self.assertEqual(reader._key, TEST_KEY)

    def test_bad_credential_formats_have_constant_corrective_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '.env'
            for contents in (
                f'KRAKEN_API_KEY={TEST_KEY}\n',
                f'KRAKEN_API_KEY={TEST_KEY}\nKRAKEN_API_KEY={TEST_KEY}\n',
                f'{TEST_SECRET}=invalid\n',
                f'KRAKEN_API_KEY={TEST_KEY}\nKRAKEN_API_SECRET="{TEST_SECRET}\n',
                f'KRAKEN_API_KEY={TEST_KEY}\nKRAKEN_API_SECRET=bad secret\n',
                'x' * 16385,
            ):
                path.write_text(contents, encoding='utf-8')
                with self.assertRaises(account.CredentialError) as error:
                    account.reader_from_credentials(path)
                self.assertNotIn(TEST_KEY, str(error.exception))
                self.assertNotIn(TEST_SECRET, str(error.exception))
            path.unlink()
            with self.assertRaises(account.CredentialError):
                account.reader_from_credentials(path)

    def test_http_authentication_failure_never_echoes_response_or_key(self):
        class RejectedTransport:
            def open(self, request, timeout):
                raise urllib.error.HTTPError(request.full_url, 401, TEST_SECRET, {}, io.BytesIO(TEST_KEY.encode()))
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        reader.client = RejectedTransport()
        with self.assertRaises(account.AccountReadError) as error:
            reader.snapshot()
        self.assertIn('permissions', str(error.exception))
        self.assertNotIn(TEST_SECRET, str(error.exception))
        self.assertNotIn(TEST_KEY, str(error.exception))

    def test_http_403_does_not_claim_invalid_credentials(self):
        class ForbiddenTransport:
            def open(self, request, timeout):
                raise urllib.error.HTTPError(request.full_url, 403, TEST_SECRET, {}, io.BytesIO(TEST_KEY.encode()))
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        reader.client = ForbiddenTransport()
        with self.assertRaises(account.AccountReadError) as error:
            reader.snapshot()
        self.assertIn('HTTP 403', str(error.exception))
        self.assertIn('key validity is not established', str(error.exception))
        self.assertNotIn(TEST_KEY, str(error.exception))
        self.assertNotIn(TEST_SECRET, str(error.exception))

    def test_account_cli_uses_internal_loader_and_separate_output(self):
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        reader.client = Transport(self.bundle)
        with tempfile.TemporaryDirectory(dir=ROOT / 'storage/kraken_shadow') as directory:
            path = Path(directory)
            with patch.object(sys, 'argv', ['kraken_account.py', '--account', '--output', str(path)]), \
                 patch.object(account, 'reader_from_credentials', return_value=reader) as load, \
                 redirect_stdout(io.StringIO()) as output:
                account.main()
            load.assert_called_once_with(ROOT / 'storage/kraken_shadow/.env', 'live')
            self.assertNotIn(TEST_KEY, output.getvalue())
            self.assertNotIn(TEST_SECRET, output.getvalue())
            report = json.loads((path / 'report.json').read_text())
            self.assertEqual(report['scope'], account.PRIVATE_SCOPE)
            self.assertTrue(report['authenticated_reads_completed'])
            self.assertFalse(report['live_account_acceptance'])
            self.assertEqual(report['account']['usd_reserve'], None)

    def test_relative_policy_account_cli_exports_report(self):
        reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
        reader.client = Transport(self.bundle)
        with tempfile.TemporaryDirectory(dir=ROOT / 'storage/kraken_shadow') as directory:
            path = Path(directory)
            policy_path = path / 'policy.json'
            policy_path.write_text(json.dumps(RELATIVE_POLICY))
            with patch.object(sys, 'argv', ['kraken_account.py', '--account', '--policy', str(policy_path),
                                           '--output', str(path)]), \
                 patch.object(account, 'reader_from_credentials', return_value=reader), \
                 redirect_stdout(io.StringIO()):
                account.main()
            result = json.loads((path / 'report.json').read_text())['account']
            self.assertEqual(result['account_equity_policy']['required_usd_reserve'], '684.90')

    def test_failed_account_cli_retains_only_safe_status(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'storage/kraken_shadow') as directory:
            path = Path(directory)
            reader = account.KrakenAccountReader(TEST_KEY, TEST_SECRET)
            with patch.object(sys, 'argv', ['kraken_account.py', '--account', '--output', str(path)]), \
                 patch.object(account, 'reader_from_credentials', return_value=reader), \
                 patch.object(reader, 'snapshot', side_effect=account.AccountReadError('Kraken rejected authentication/access at permissions')):
                with self.assertRaises(SystemExit):
                    account.main()
            rendered = (path / 'last_attempt.json').read_text()
            self.assertNotIn(TEST_KEY, rendered)
            self.assertNotIn(TEST_SECRET, rendered)
            self.assertEqual(json.loads(rendered)['outcome'], 'FAILED')
            self.assertFalse((path / 'report.json').exists())


def study(directory):
    directory = directory.resolve()
    if not directory.is_relative_to((ROOT / 'storage/kraken_shadow').resolve()):
        raise ValueError('Keep study artifacts under ignored storage/kraken_shadow')
    now = time.time()
    bundle = fixture(now)
    journal = account.AccountJournal(directory, account.FIXTURE_SCOPE, 'live', POLICY)
    try:
        result = journal.observe(bundle, now)
        account.write_json(directory / 'fixture.json', bundle)
        account.write_json(directory / 'policy.json', POLICY)
        account.export_report(directory, result)
    finally:
        journal.close()
    print('Synthetic study:', directory / 'report.html')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--study', type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AccountTest)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    if args.study:
        study(args.study)
