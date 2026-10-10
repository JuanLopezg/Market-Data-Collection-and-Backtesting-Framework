#!/usr/bin/env python3
"""Read-only Kraken Multi-M account inspection with separate fixture/account modes.

Credentials are consumed internally without logging their contents. This component never
submits orders, changes settlement preferences or writes trading account state.
"""
import argparse
import base64
from datetime import datetime
from decimal import Decimal
import hashlib
import hmac
import html
import json
from pathlib import Path
import re
import sqlite3
import time
import urllib.request
import urllib.error

from kraken_shadow import HOSTS, ROOT, fresh, write_json
from shadow_observer import NoRedirect, canonical, digest, number


FIXTURE_SCOPE = 'LOCAL_KRAKEN_ACCOUNT_FIXTURE'
PRIVATE_SCOPE = 'KRAKEN_AUTHENTICATED_READ_ONLY'
ROUTES = {
    'permissions': ('/api/auth/v1/api-keys/v3/check', '/api/auth/v1/api-keys/v3/check'),
    'wallets': ('/derivatives/api/v3/accounts', '/api/v3/accounts'),
    'positions': ('/derivatives/api/v3/openpositions', '/api/v3/openpositions'),
    'orders': ('/derivatives/api/v3/openorders', '/api/v3/openorders'),
    'preferences': ('/derivatives/api/v3/pnlpreferences', '/api/v3/pnlpreferences'),
}


class CredentialError(ValueError):
    """A constant corrective message, never file contents or credential values."""


class AccountReadError(ValueError):
    """A bounded endpoint/status diagnostic, never exchange bodies or headers."""


def reader_from_credentials(path, environment='live'):
    """Read just the dedicated credential file inside the process, not via a shell."""
    try:
        if path.is_symlink():
            raise CredentialError('Use a regular credential file, not a symbolic link')
        with path.open('rb') as stream:
            raw = stream.read(16385)
        if len(raw) > 16384:
            raise CredentialError('Credential file must contain only the two Kraken variables')
        contents = raw.decode('utf-8-sig')
    except (OSError, UnicodeError):
        raise CredentialError('Create storage/kraken_shadow/.env as a UTF-8 text file') from None
    values = {}
    expected = ('KRAKEN_API_KEY', 'KRAKEN_API_SECRET')
    for line in contents.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, separator, value = line.partition('=')
        name, value = name.strip(), value.strip()
        if not separator or name not in expected or name in values:
            raise CredentialError('Use KRAKEN_API_KEY=value and KRAKEN_API_SECRET=value, once each')
        if value.startswith(('"', "'")):
            if len(value) < 2 or value[-1] != value[0]:
                raise CredentialError('Close the quotes around each credential value')
            value = value[1:-1]
        if not value or any(character.isspace() for character in value):
            raise CredentialError('Both Kraken values must be nonempty single-line values without spaces')
        values[name] = value
    if set(values) != set(expected):
        raise CredentialError('Add both KRAKEN_API_KEY and KRAKEN_API_SECRET to the dedicated .env')
    try:
        return KrakenAccountReader(values['KRAKEN_API_KEY'], values['KRAKEN_API_SECRET'], environment)
    except ValueError:
        raise CredentialError('Check the copied public key and Base64 private secret; do not paste them into chat') from None


def read_only_permissions(response):
    permissions = response['permissions']
    if permissions.get('general') != 'READ_ONLY' or permissions.get('transfer') != 'NO_ACCESS':
        raise ValueError('Use a read-only Derivatives key without transfer access')
    if any(value != 'NO_ACCESS' for key, value in permissions.items() if key != 'general'):
        raise ValueError('Additional API permissions are not permitted')
    account_uid = response['accountUid']
    if not isinstance(account_uid, str) or not account_uid:
        raise ValueError('Missing authenticated account identity')
    # Do not copy the key, IIBAN, IP restrictions or account UID into reports.
    return {'general': 'READ_ONLY', 'transfer': 'NO_ACCESS',
            'account_identity_hash': digest({'kraken_account_uid': account_uid})}


class KrakenAccountReader:
    """Five fixed GET routes. No generic HTTP request or mutation interface."""

    def __init__(self, api_key, api_secret, environment='live'):
        if not isinstance(api_key, str) or not re.fullmatch(r'[A-Za-z0-9+/=_-]{8,256}', api_key):
            raise ValueError('Invalid Derivatives API key format')
        try:
            secret = base64.b64decode(api_secret, validate=True)
            if len(secret) < 16:
                raise ValueError('Invalid secret size')
        except (ValueError, TypeError):
            raise ValueError('Invalid Derivatives API secret format') from None
        self._key = api_key
        self._secret = secret
        self.environment = environment
        self._host = HOSTS[environment]
        self._permissions = None
        self.client = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def _get(self, route):
        if route not in ROUTES or (route != 'permissions' and self._permissions is None):
            raise ValueError('Only permission-checked account reads are permitted')
        path, signing_path = ROUTES[route]
        # These GETs have no query/body. Kraken permits omitting nonce. Therefore
        # postData and nonce are both empty, including after a client restart.
        hashed = hashlib.sha256(signing_path.encode()).digest()
        signature = base64.b64encode(hmac.new(self._secret, hashed, hashlib.sha512).digest()).decode()
        request = urllib.request.Request(self._host + path, method='GET',
            headers={'APIKey': self._key, 'Authent': signature, 'Accept': 'application/json',
                     'User-Agent': 'algoTrading-read-only-account'})
        try:
            with self.client.open(request, timeout=10) as response:
                body = response.read(2 * 1024 * 1024 + 1)
                if len(body) > 2 * 1024 * 1024:
                    raise ValueError('Account response exceeds its bound')
                value = json.loads(body, parse_float=Decimal)
                if not isinstance(value, dict):
                    raise ValueError('Invalid account response')
                return value
        except urllib.error.HTTPError as error:
            if error.code == 403:
                raise AccountReadError('Kraken access forbidden (HTTP 403) at ' + route +
                    '; check network/IP or account restrictions; key validity is not established') from None
            if error.code == 401:
                raise AccountReadError('Kraken rejected authentication/access (HTTP ' + str(error.code) + ') at ' + route +
                    '; check Futures keys, permissions and IP restrictions') from None
            raise AccountReadError('Kraken HTTP failure at ' + route) from None
        except (OSError, ValueError):
            # HTTP/JSON errors may embed keys, headers or response bodies.
            raise AccountReadError('Kraken account read unavailable or invalid at ' + route) from None

    def snapshot(self):
        self._permissions = None
        checked = self._get('permissions')
        if not isinstance(checked.get('apiKey'), str) or not hmac.compare_digest(checked['apiKey'], self._key):
            raise AccountReadError('API permission response did not match the supplied key')
        try:
            self._permissions = read_only_permissions(checked)
        except (KeyError, ValueError, TypeError, AttributeError):
            raise AccountReadError('Set General API to Read Only and Withdrawal API to No Access') from None
        permission_time = time.time()
        responses = {route: self._get(route) for route in ('wallets', 'positions', 'orders', 'preferences')}
        bundle = {'scope': PRIVATE_SCOPE, 'environment': self.environment, 'observed_at': time.time(),
                  'permission_checked_at': permission_time, 'permissions': self._permissions, **responses}
        # A bundle of sequential reads is not an atomic exchange transaction.
        validate_bundle(bundle, bundle['observed_at'])
        return bundle


def validate_bundle(bundle, now):
    if bundle['scope'] not in (FIXTURE_SCOPE, PRIVATE_SCOPE) or bundle['environment'] not in HOSTS:
        raise ValueError('Invalid Kraken account source')
    fresh(bundle['observed_at'], now)
    fresh(bundle['permission_checked_at'], now)
    permissions = bundle['permissions']
    if permissions['general'] != 'READ_ONLY' or permissions['transfer'] != 'NO_ACCESS':
        raise ValueError('Account bundle is not read-only')
    if not re.fullmatch('[0-9a-f]{64}', permissions['account_identity_hash']):
        raise ValueError('Missing account identity fingerprint')
    clocks = []
    for route in ('wallets', 'positions', 'orders', 'preferences'):
        response = bundle[route]
        if response.get('result') != 'success':
            raise AccountReadError('Account endpoint did not succeed at ' + route)
        try:
            clock = utc_timestamp(response['serverTime'])
        except (KeyError, ValueError, TypeError, AttributeError):
            raise AccountReadError('Missing or invalid UTC server clock at ' + route) from None
        try:
            fresh(clock, now)
        except ValueError:
            raise AccountReadError('Stale or future account server clock at ' + route) from None
        clocks.append(clock)
    if max(clocks) - min(clocks) > 5:
        raise AccountReadError('Account endpoint reads exceed the permitted five-second time spread')
    return clocks


def utc_timestamp(value):
    parsed = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if parsed.utcoffset() is None or parsed.utcoffset().total_seconds() != 0:
        raise ValueError('Account timestamps must include UTC timezone')
    return parsed.timestamp()


def required_rows(response, field):
    rows = response[field]
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise ValueError('Missing account row list')
    return rows


def economic_fields(row, keys, nonnegative=()):
    values = {key: number(row[key]) for key in keys}
    if any(values[key] < 0 for key in nonnegative):
        raise ValueError('Negative balance/size/margin requirement')
    return {key: str(value) for key, value in values.items()}


def currency_name(value):
    if not isinstance(value, str) or not re.fullmatch('[A-Za-z0-9]{1,16}', value):
        raise ValueError('Invalid collateral currency')
    return 'BTC' if value.upper() == 'XBT' else value.upper()


def settlement_preferences(response):
    result = {}
    for entry in required_rows(response, 'preferences'):
        symbol = entry['symbol']
        if not isinstance(symbol, str) or symbol in result:
            raise ValueError('Invalid or duplicate PnL preference')
        result[symbol] = currency_name(entry['pnlCurrency'])
    return result


def normalize_account(bundle, policy, now):
    clocks = validate_bundle(bundle, now)
    wallet = bundle['wallets']['accounts']['flex']
    if wallet['type'] != 'multiCollateralMarginAccount':
        raise ValueError('Supported Multi-M wallet unavailable; do not use another wallet')
    currencies = {}
    if not isinstance(wallet['currencies'], dict):
        raise ValueError('Missing collateral currency mapping')
    for code, row in wallet['currencies'].items():
        code = currency_name(code)
        if code in currencies:
            raise ValueError('Duplicate collateral currency alias')
        currencies[code] = economic_fields(row, ('quantity', 'value', 'collateral', 'available'))
    totals = economic_fields(wallet, ('collateralValue', 'marginEquity', 'availableMargin',
        'initialMargin', 'initialMarginWithOrders', 'maintenanceMargin', 'pnl', 'unrealizedFunding',
        'totalUnrealized', 'totalUnrealizedAsMargin'),
        ('initialMargin', 'initialMarginWithOrders', 'maintenanceMargin'))
    if number(totals['initialMarginWithOrders']) < number(totals['initialMargin']):
        raise ValueError('Order-inclusive margin is smaller than position margin')
    preferences = settlement_preferences(bundle['preferences'])
    positions, symbols = [], set()
    reasons = []
    for entry in required_rows(bundle['positions'], 'openPositions'):
        symbol, side = entry['symbol'], entry['side']
        if not isinstance(symbol, str) or symbol in symbols or side not in ('long', 'short'):
            raise ValueError('Invalid or duplicate account position')
        symbols.add(symbol)
        values = economic_fields(entry, ('size', 'price', 'unrealizedPnl'), ('size', 'price'))
        if number(values['size']) == 0 or number(values['price']) == 0:
            raise ValueError('Open position size and price must be positive')
        values['signed_quantity'] = str(number(values['size']) * (1 if side == 'long' else -1))
        reported_currency = currency_name(entry['pnlCurrency']) if entry.get('pnlCurrency') else None
        preference = preferences.get(symbol, 'USD')
        if reported_currency is not None and reported_currency != preference:
            reasons.append('POSITION_SETTLEMENT_PREFERENCE_CONFLICT')
        funding = entry.get('unrealizedFunding')
        positions.append({'symbol': symbol, 'side': side, **values,
            'unrealizedFunding': str(number(funding)) if funding is not None else None,
            'pnl_currency': reported_currency or preference})
        if not symbol.startswith('PF_'):
            reasons.append('OTHER_DERIVATIVE_PRODUCT_REQUIRES_REVIEW')
    orders, identities = [], set()
    for entry in required_rows(bundle['orders'], 'openOrders'):
        identity = entry['order_id']
        if not isinstance(identity, str) or not identity or identity in identities or entry['side'] not in ('buy', 'sell'):
            raise ValueError('Invalid or duplicate account order')
        identities.add(identity)
        values = economic_fields(entry, ('filledSize', 'unfilledSize'), ('filledSize', 'unfilledSize'))
        if not isinstance(entry['reduceOnly'], bool):
            raise ValueError('Invalid reduce-only flag')
        if not isinstance(entry['symbol'], str) or not entry['symbol']:
            raise ValueError('Missing order symbol')
        received = utc_timestamp(entry['receivedTime'])
        updated = utc_timestamp(entry['lastUpdateTime'])
        if updated < received or updated > now + 5:
            raise ValueError('Invalid order lifecycle timestamps')
        orders.append({'order_id': identity, 'symbol': entry['symbol'], 'side': entry['side'], **values,
            'reduce_only': entry['reduceOnly'], 'last_update': updated,
            'partially_filled': number(values['filledSize']) > 0 and number(values['unfilledSize']) > 0})
        if not entry['symbol'].startswith('PF_'):
            reasons.append('OTHER_DERIVATIVE_PRODUCT_REQUIRES_REVIEW')
    if any(code not in ('BTC', 'USD') and number(row['quantity']) != 0 for code, row in currencies.items()):
        reasons.append('OTHER_COLLATERAL_REQUIRES_REVIEW')
    if 'BTC' in currencies and number(currencies['BTC']['quantity']) < 0:
        reasons.append('BTC_BALANCE_NEGATIVE')
    if any(currency != 'USD' for currency in preferences.values()) or any(row['pnl_currency'] != 'USD' for row in positions):
        reasons.append('NON_USD_PROFIT_SETTLEMENT')
    # Use venue totals directly: PnL/funding are already reflected in marginEquity.
    initial_headroom = number(totals['marginEquity']) - number(totals['initialMarginWithOrders'])
    maintenance_headroom = number(totals['marginEquity']) - number(totals['maintenanceMargin'])
    if initial_headroom < 0 or number(totals['availableMargin']) < 0:
        reasons.append('INITIAL_MARGIN_SHORTFALL')
    if maintenance_headroom < 0:
        reasons.append('MAINTENANCE_MARGIN_BREACH')
    reserve = None
    equity = number(totals['marginEquity'])
    equity_policy = None
    if policy is not None:
        allowed = {'minimum_usd_reserve', 'minimum_account_equity_usd',
                   'minimum_usd_reserve_fraction', 'assumed_future_cash_debits_usd'}
        if not isinstance(policy, dict) or set(policy) - allowed:
            raise ValueError('Unknown reserve policy field')
        relative = 'minimum_usd_reserve_fraction' in policy
        if relative != ('minimum_account_equity_usd' in policy):
            raise ValueError('Account minimum and USD fraction must be specified together')
        if not relative and 'minimum_usd_reserve' not in policy:
            raise ValueError('Missing USD reserve threshold')
        absolute_minimum = number(policy.get('minimum_usd_reserve', 0))
        fraction = number(policy.get('minimum_usd_reserve_fraction', 0))
        cash_debits = number(policy['assumed_future_cash_debits_usd'])
        if absolute_minimum < 0 or cash_debits < 0 or fraction < 0 or fraction > 1:
            raise ValueError('Negative USD reserve policy')
        # The denominator is wallet equity, never leveraged position notional.
        minimum = max(absolute_minimum, max(Decimal(0), equity) * fraction)
        if relative:
            minimum_equity = number(policy['minimum_account_equity_usd'])
            if minimum_equity < 0:
                raise ValueError('Negative account equity minimum')
            equity_policy = {'basis': 'VENUE_MARGIN_EQUITY_USD',
                'minimum_account_equity_usd': str(minimum_equity),
                'minimum_usd_reserve_fraction': str(fraction),
                'account_equity_usd': str(equity), 'required_usd_reserve': str(minimum)}
            if equity < minimum_equity:
                reasons.append('ACCOUNT_EQUITY_BELOW_MINIMUM')
    if policy is None:
        reasons.append('USD_RESERVE_POLICY_NOT_CONFIGURED')
    elif 'USD' not in currencies:
        reasons.append('USD_BALANCE_NOT_REPORTED')
    else:
        usd = min(number(currencies['USD']['quantity']), number(currencies['USD']['available']))
        loss = max(Decimal(0), -number(totals['totalUnrealized']))
        remaining = usd - cash_debits - loss
        reserve = {'minimum_usd_reserve': str(minimum), 'assumed_future_cash_debits_usd': str(cash_debits),
                   'usd_available_now': str(usd),
                   'usd_after_assumed_debits_and_unrealized_loss': str(remaining)}
        if usd < minimum or (not relative and remaining < minimum):
            reasons.append('USD_RESERVE_LOW')
        if relative and remaining < minimum:
            reasons.append('USD_RESERVE_LOW_AFTER_LOSS_STRESS')
        if usd - cash_debits < 0:
            reasons.append('USD_DEBITS_REQUIRE_CONVERSION')
        if loss > max(Decimal(0), usd - cash_debits):
            reasons.append('UNREALIZED_LOSS_NOT_BACKED_BY_USD')
    return {'scope': bundle['scope'], 'environment': bundle['environment'],
        'account_identity_hash': bundle['permissions']['account_identity_hash'],
        'observed_at': bundle['observed_at'], 'server_times': clocks,
        'wallet_type': 'multiCollateralMarginAccount', 'currencies': currencies, 'venue_totals_usd': totals,
        'positions': positions, 'orders': orders, 'pnl_preferences': preferences, 'usd_reserve': reserve,
        'account_equity_policy': equity_policy,
        'initial_margin_headroom_with_orders_usd': str(initial_headroom),
        'maintenance_margin_headroom_usd': str(maintenance_headroom),
        'state': 'REVIEW_REQUIRED' if reasons else 'READ_ONLY_CHECKS_PASS', 'reasons': sorted(set(reasons)),
        'can_trade': None, 'submitted': False, 'btc_sold': False,
        'atomic_exchange_snapshot': False, 'core_account_updated': False,
        'live_account_acceptance': False}


class AccountJournal:
    """Persist observations only; bind account/environment/scope and reserve policy."""

    def __init__(self, directory, scope, environment, policy):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.policy = policy
        self.scope, self.environment = scope, environment
        self.db = sqlite3.connect(self.directory / 'account-observations.sqlite')
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS context(id INTEGER PRIMARY KEY CHECK(id=1), identity TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS samples(id TEXT PRIMARY KEY, observed_at REAL NOT NULL, result TEXT NOT NULL);
        ''')

    def close(self):
        self.db.close()

    def observe(self, bundle, now):
        if bundle['scope'] != self.scope or bundle['environment'] != self.environment:
            raise ValueError('Account journal source/environment mismatch')
        result = normalize_account(bundle, self.policy, now)
        identity = canonical({'account': result['account_identity_hash'], 'scope': self.scope,
                              'environment': self.environment, 'policy': self.policy})
        sample_id = digest({'account': result['account_identity_hash'], 'observed_at': result['observed_at']})
        with self.db:
            context = self.db.execute('SELECT identity FROM context WHERE id=1').fetchone()
            if context and context[0] != identity:
                raise ValueError('Account journal belongs to another account, source or reserve policy')
            if not context:
                self.db.execute('INSERT INTO context VALUES (1,?)', (identity,))
            existing = self.db.execute('SELECT result FROM samples WHERE id=?', (sample_id,)).fetchone()
            if existing:
                if existing[0] != canonical(result):
                    raise ValueError('Captured account observation changed')
                return json.loads(existing[0])
            latest = self.db.execute('SELECT max(observed_at) FROM samples').fetchone()[0]
            if latest is not None and bundle['observed_at'] < latest:
                raise ValueError('Out-of-order account sample')
            self.db.execute('INSERT INTO samples VALUES (?,?,?)', (sample_id, bundle['observed_at'], canonical(result)))
        return result


def export_report(directory, result):
    authenticated = result['scope'] == PRIVATE_SCOPE
    report = {'scope': result['scope'], 'submitted': False, 'account': result,
              'credential_loading': 'INTERNAL_ONLY' if authenticated else 'NOT_USED',
              'authenticated_reads_completed': authenticated, 'live_account_acceptance': False,
              'pending': ([] if authenticated else ['Actual credentials/account validation']) + ['Supported private test environment',
                          'Remaining stop controls, funding/cost accounting and forward shadow']}
    # Own wording: the public study report must not mislabel a private/fixture wallet.
    write_json(directory / 'report.json', report)
    page = ('<!doctype html><html lang="en"><meta charset="utf-8"><title>Kraken account inspection</title>'
        '<style>body{font:17px system-ui;margin:32px;max-width:1200px}pre{white-space:pre-wrap}</style>'
        '<h1>Kraken BTC/USD account inspection</h1><p>Read-only observations. '
        'LOCAL_KRAKEN_ACCOUNT_FIXTURE means synthetic balances, not account access. '
        'Margin values come from the supplied venue response, including existing orders. '
        'No trading approval, orders or BTC sales.</p><pre>'
        + html.escape(json.dumps(report, indent=2, ensure_ascii=False)) + '</pre></html>')
    (directory / 'report.html').write_text(page, encoding='utf-8')
    return report


def record_attempt(directory, environment, outcome, diagnostic):
    """Retain only our own status wording; never retain raw authentication replies."""
    try:
        directory.mkdir(parents=True, exist_ok=True)
        write_json(directory / 'last_attempt.json', {
            'scope': PRIVATE_SCOPE, 'environment': environment, 'observed_at': time.time(),
            'outcome': outcome, 'diagnostic': diagnostic, 'submitted': False})
    except OSError:
        # Reporting failure must not produce a traceback containing sensitive context.
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--fixture', type=Path, help='Explicit synthetic account bundle, not credentials')
    mode.add_argument('--account', action='store_true', help='Read real account using the dedicated ignored .env internally')
    parser.add_argument('--environment', choices=HOSTS, default='live')
    parser.add_argument('--policy', type=Path, help='Explicit account minimum and USD reserve policy')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    directory = (args.output or ROOT / 'storage/kraken_shadow' /
                 ('account-' + args.environment if args.account else 'account-fixture')).resolve()
    if not directory.is_relative_to((ROOT / 'storage/kraken_shadow').resolve()):
        parser.error('Keep account study state under ignored storage/kraken_shadow')
    phase = 'reading policy'
    try:
        policy = json.loads(args.policy.read_text(encoding='utf-8')) if args.policy else None
        phase = 'reading account snapshot'
        if args.account:
            reader = reader_from_credentials(ROOT / 'storage/kraken_shadow/.env', args.environment)
            bundle = reader.snapshot()
        else:
            bundle = json.loads(args.fixture.read_text(encoding='utf-8'))
            if bundle['scope'] != FIXTURE_SCOPE:
                raise ValueError('Fixture mode requires an explicitly synthetic fixture')
        phase = 'validating account and journal'
        journal = AccountJournal(directory, bundle['scope'], bundle['environment'], policy)
        try:
            result = journal.observe(bundle, time.time())
            phase = 'exporting account report'
            export_report(directory, result)
        finally:
            journal.close()
    except (CredentialError, AccountReadError) as error:
        if args.account:
            record_attempt(directory, args.environment, 'FAILED', str(error))
        raise SystemExit('KRAKEN-ACCOUNT: ' + str(error)) from None
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError, AttributeError):
        if args.account:
            record_attempt(directory, args.environment, 'FAILED', 'Unavailable or invalid input while ' + phase)
        raise SystemExit('KRAKEN-ACCOUNT: unavailable or invalid input while ' + phase + '; no trading account update') from None
    if args.account:
        record_attempt(directory, args.environment, 'READ_ONLY_COMPLETED', 'Authenticated observations retained; trading not approved')
    print('KRAKEN-ACCOUNT:', 'authenticated read-only inspection complete' if args.account else 'synthetic fixture inspection complete')
    print('Wallet:', result['wallet_type'], '| Currencies:', ', '.join(sorted(result['currencies'])))
    print('Positions:', len(result['positions']), '| Open orders:', len(result['orders']))
    print('Checks:', result['state'], '| Reasons:', ', '.join(result['reasons']) or 'none')
    print('Report:', directory / 'report.html')


if __name__ == '__main__':
    main()
