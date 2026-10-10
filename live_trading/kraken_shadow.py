#!/usr/bin/env python3
"""Public Kraken Multi-M plan observations and hypothetical BTC/USD collateral.

No keys, private routes, order submissions, wallet transfers or simulated fills.
Exported plans remain immutable. Collateral scenarios are never account balances.
"""
import argparse
from datetime import datetime, timezone
from decimal import Decimal, ROUND_DOWN
import hashlib
import html
import json
from pathlib import Path
import time
import urllib.request

from shadow_observer import NoRedirect, ShadowJournal, digest, number, validate_plan


ROOT = Path(__file__).resolve().parents[1]
HOSTS = {'live': 'https://futures.kraken.com', 'demo': 'https://demo-futures.kraken.com'}
SCOPE = 'KRAKEN_PUBLIC_READ_ONLY'
BTC_HAIRCUT = Decimal('0.01')
RULES = {
    'reviewed_on': '2026-10-10',
    'btc_haircut': str(BTC_HAIRCUT),
    'btc_conversion_fee': '0.002',
    'collateral': 'https://support.kraken.com/articles/collateral-currencies-eea',
    'charges': 'https://support.kraken.com/articles/4844392809620-fees-charges-for-multi-collateral-derivatives',
    'eligibility': 'https://support.kraken.com/articles/derivatives-eligibility-requirements-eea',
    'instruments': 'https://docs.kraken.com/api-reference/instrument-details/get-instruments',
}


def fresh(observed_at, now, maximum_age=30):
    age = number(now) - number(observed_at)
    if age < -5 or age > maximum_age:
        raise ValueError('Kraken snapshot is stale or ahead of the clock')


def server_time(response):
    return datetime.fromisoformat(response['serverTime'].replace('Z', '+00:00')).timestamp()


def unique_rows(rows):
    if not isinstance(rows, list) or not rows:
        raise ValueError('Missing Kraken catalogue rows')
    result = {}
    for row in rows:
        symbol = row['symbol']
        if symbol in result:
            raise ValueError('Duplicate Kraken market identity')
        result[symbol] = row
    return result


def quoted_number(value):
    if value is None:
        return None
    parsed = number(value)
    if parsed < 0:
        raise ValueError('Negative Kraken price')
    return str(parsed) if parsed > 0 else None


def normalize(instruments, tickers, now, environment):
    """Keep inactive contracts visible; missing/malformed metadata is not a skip."""
    if instruments.get('result') != 'success' or tickers.get('result') != 'success':
        raise ValueError('Kraken public request did not succeed')
    for response in (instruments, tickers):
        fresh(server_time(response), now)
    prices = unique_rows(tickers['tickers'])
    markets = {}
    for symbol, instrument in unique_rows(instruments['instruments']).items():
        if not symbol.startswith('PF_') or instrument.get('tradfi') is not False:
            continue
        if instrument['type'] != 'flexible_futures' or instrument['quote'] != 'USD':
            raise ValueError('Unexpected Kraken Multi-M contract type or currency')
        base = instrument['base']
        base = 'BTC' if base == 'XBT' else base
        if base in markets:
            raise ValueError('Ambiguous Kraken underlying market')
        expired = instrument['isExpired']
        tradeable = instrument['tradeable']
        if not isinstance(expired, bool) or not isinstance(tradeable, bool):
            raise ValueError('Invalid Kraken market status')
        ticker = prices.get(symbol)
        if ticker is None and tradeable and not expired:
            raise ValueError('Active Kraken contract has no ticker')
        if ticker is not None and (ticker['tag'] != 'perpetual' or not isinstance(ticker['suspended'], bool)):
            raise ValueError('Invalid Kraken perpetual ticker status')
        trading = tradeable and not expired and ticker is not None and not ticker['suspended']
        row = {'symbol': symbol, 'trading': trading, 'quote_currency': 'USD',
               'platforms_permitted': instrument.get('platformsPermitted'),
               'countries_banned': instrument.get('countriesBanned'),
               'margin_schedules': instrument.get('marginSchedules'),
               'account_eligibility': 'UNVERIFIED'}
        if trading:
            if not isinstance(instrument['postOnly'], bool) or not isinstance(ticker['postOnly'], bool):
                raise ValueError('Invalid Kraken post-only status')
            precision = instrument['contractValueTradePrecision']
            if isinstance(precision, bool) or not isinstance(precision, int) or not -12 <= precision <= 12:
                raise ValueError('Invalid Kraken quantity precision')
            if number(instrument['contractSize'], True) != 1:
                raise ValueError('Unsupported Kraken contract-unit conversion')
            row.update(quantity_step=str(Decimal(10) ** -precision),
                       tick_size=str(number(instrument['tickSize'], True)),
                       mark_price=quoted_number(ticker.get('markPrice')),
                       index_price=quoted_number(ticker.get('indexPrice')),
                       bid=quoted_number(ticker.get('bid')), ask=quoted_number(ticker.get('ask')),
                       post_only=instrument['postOnly'] or ticker['postOnly'])
            if row['bid'] is not None and row['ask'] is not None and number(row['bid']) > number(row['ask']):
                raise ValueError('Crossed Kraken quote')
        markets[base] = row
    if not markets or 'BTC' not in markets or not markets['BTC']['trading']:
        raise ValueError('Complete Kraken catalogue/BTC collateral index unavailable')
    number(markets['BTC']['index_price'], True)
    snapshot = {'venue': 'KRAKEN_MULTI_M', 'scope': SCOPE, 'environment': environment,
            'complete': True, 'observed_at': now, 'server_times': [server_time(instruments), server_time(tickers)],
            'markets': markets, 'btc_index_price_usd': markets['BTC']['index_price']}
    return json.loads(json.dumps(snapshot, default=str))


class KrakenPublicReader:
    """Fixed HTTPS hosts and two GET-only public routes; redirects/proxies disabled."""

    def __init__(self, environment, directory):
        self.host = HOSTS[environment]
        self.environment = environment
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.client = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def read(self, route):
        if route not in ('instruments', 'tickers'):
            raise ValueError('Only Kraken public instruments/tickers are permitted')
        url = self.host + '/derivatives/api/v3/' + route
        request = urllib.request.Request(url, method='GET', headers={'User-Agent': 'algoTrading-public-shadow'})
        try:
            with self.client.open(request, timeout=10) as response:
                body = response.read(10 * 1024 * 1024 + 1)
                if len(body) > 10 * 1024 * 1024:
                    raise ValueError('Kraken public response exceeds its bound')
                data = json.loads(body, parse_float=Decimal)
                # Preserve JSON bytes exactly; Decimal values are parsed again on audit.
                (self.directory / (route + '.json')).write_bytes(body)
                envelope = {'method': 'GET', 'url': url, 'retrieved_at': time.time(),
                            'http_date': response.headers.get('Date'), 'sha256': hashlib.sha256(body).hexdigest()}
                write_json(self.directory / (route + '-receipt.json'), envelope)
                return data
        except (OSError, ValueError) as error:
            raise ValueError('Kraken public endpoint unavailable or invalid') from error

    def snapshot(self):
        instruments = self.read('instruments')
        tickers = self.read('tickers')
        return normalize(instruments, tickers, time.time(), self.environment)


def source_asset(coin):
    if not coin.endswith('USDT') or len(coin) <= 4:
        raise ValueError('Explicit Binance USDT source symbol required')
    base = coin[:-4]
    # Unit scaling is relevant to the source close, not to USD monetary intent.
    aliases = {'1000PEPE': 'PEPE', '1000SHIB': 'SHIB', '1000BONK': 'BONK',
               '1000FLOKI': 'FLOKI', '1000LUNC': 'LUNC', '1000DOGS': 'DOGS',
               '1000SATS': 'SATS', '1000CHEEMS': 'CHEEMS', '1000000MOG': 'MOG'}
    return aliases.get(base, base)


def assess_kraken(plan, snapshot, now):
    validate_plan(plan, now)
    if snapshot.get('venue') != 'KRAKEN_MULTI_M' or snapshot.get('complete') is not True:
        raise ValueError('Incomplete or wrong-source Kraken snapshot')
    fresh(snapshot['observed_at'], now)
    for timestamp in snapshot['server_times']:
        fresh(timestamp, now)
    observations = []
    for order in plan['submit_orders']:
        base = source_asset(order['coin'])
        row = {'economic_order_id': order['economic_order_id'], 'coin': order['coin'], 'asset': base,
               'side': 'BUY' if order['side'] == 0 else 'SELL', 'notional_usd': order['notional_usd'],
               'source_reference_close': order['reference_close'], 'submitted': False,
               'state': 'SKIPPED', 'reason': 'MULTI_M_PERPETUAL_UNAVAILABLE', 'account_admission': 'UNVERIFIED'}
        market = snapshot['markets'].get(base)
        if market is not None:
            row['kraken_symbol'] = market['symbol']
            if not market['trading']:
                row['reason'] = 'MULTI_M_PERPETUAL_NOT_TRADING'
            else:
                if any(market[key] is None for key in ('mark_price', 'bid', 'ask')):
                    raise ValueError('Selected Kraken market has unavailable prices/order book')
                mark = number(market['mark_price'], True)
                quantity = number(order['notional_usd'], True) / mark
                step = number(market['quantity_step'], True)
                rounded = (quantity / step).to_integral_value(rounding=ROUND_DOWN) * step
                row.update(state='OBSERVED' if rounded >= step else 'SKIPPED',
                           reason='PUBLIC_QUOTE_ONLY' if rounded >= step else 'BELOW_MINIMUM_QUANTITY',
                           hypothetical_quantity_at_current_mark=str(rounded), observed_mark_usd=str(mark),
                           observed_notional_usd=str(rounded * mark), quantity_step=str(step),
                           bid=market['bid'], ask=market['ask'], post_only=market['post_only'])
                # Never convert the historical Binance close to USD or reuse its
                # base-contract quantity: scaled products and current prices differ.
        observations.append(row)
    return {'state': 'OBSERVED' if observations or plan['cancel_order_ids'] else 'NO_ORDERS',
            'orders': observations, 'cancel_order_ids_not_sent': plan['cancel_order_ids'],
            'venue_observed_at': snapshot['observed_at'], 'decision_timestamp': plan['decision_timestamp'],
            'scope': SCOPE, 'environment': snapshot['environment'], 'submitted': False,
            'venue_snapshot_hash': digest(snapshot),
            'fills': None, 'pnl': None, 'fees': None, 'funding': None,
            'collateral': None, 'quote_to_usd_conversion': None}


def collateral_scenario(scenario, snapshot, now):
    """Diagnostic for an explicit hypothetical cross-margin BTC/USD wallet only."""
    if scenario['scope'] != 'HYPOTHETICAL_CROSS_MARGIN_SCENARIO' or scenario['profit_currency'] != 'USD':
        raise ValueError('Use an explicit hypothetical cross-margin scenario with USD settlement')
    if snapshot.get('venue') != 'KRAKEN_MULTI_M' or snapshot.get('complete') is not True:
        raise ValueError('Incomplete or wrong-source Kraken collateral index')
    fresh(snapshot['observed_at'], now)
    for timestamp in snapshot['server_times']:
        fresh(timestamp, now)
    values = {key: number(scenario[key]) for key in ('btc_balance', 'usd_balance', 'unrealized_pnl_usd',
              'unrealized_funding_usd', 'initial_margin_usd', 'maintenance_margin_usd',
              'cash_debits_usd', 'minimum_usd_reserve')}
    for key in ('btc_balance', 'initial_margin_usd', 'maintenance_margin_usd', 'cash_debits_usd', 'minimum_usd_reserve'):
        if values[key] < 0:
            raise ValueError('Scenario balance/margin/debit/reserve cannot be negative')
    if values['maintenance_margin_usd'] > values['initial_margin_usd']:
        raise ValueError('Maintenance margin exceeds initial margin')
    price = number(snapshot['btc_index_price_usd'], True)
    btc_value = values['btc_balance'] * price
    adjusted_btc = btc_value * (1 - BTC_HAIRCUT)
    usd_after_debits = values['usd_balance'] - values['cash_debits_usd']
    unrealized = values['unrealized_pnl_usd'] + values['unrealized_funding_usd']
    equity = adjusted_btc + usd_after_debits + unrealized
    uncovered = max(Decimal(0), -unrealized - max(Decimal(0), usd_after_debits))
    reserve_after_losses = usd_after_debits - max(Decimal(0), -unrealized)
    reasons = []
    if reserve_after_losses < values['minimum_usd_reserve']:
        reasons.append('USD_RESERVE_LOW')
    if usd_after_debits < 0:
        reasons.append('USD_DEBITS_REQUIRE_CONVERSION')
    if uncovered > 0:
        reasons.append('UNREALIZED_LOSS_NOT_BACKED_BY_USD')
    if equity < values['initial_margin_usd']:
        reasons.append('INITIAL_MARGIN_SHORTFALL')
    if equity < values['maintenance_margin_usd']:
        reasons.append('MAINTENANCE_MARGIN_BREACH')
    return {'scope': scenario['scope'], 'input': scenario, 'btc_index_price_usd': str(price),
            'btc_market_value_usd': str(btc_value), 'btc_collateral_after_haircut_usd': str(adjusted_btc),
            'usd_after_assumed_cash_debits': str(usd_after_debits),
            'usd_reserve_after_unrealized_losses': str(reserve_after_losses),
            'estimated_margin_equity_usd': str(equity), 'unbacked_unrealized_loss_usd': str(uncovered),
            'estimated_initial_margin_headroom_usd': str(equity - values['initial_margin_usd']),
            'estimated_maintenance_margin_headroom_usd': str(equity - values['maintenance_margin_usd']),
            'state': 'REVIEW_REQUIRED' if reasons else 'SCENARIO_CHECKS_PASS', 'reasons': reasons,
            'can_trade': None, 'btc_sold': False, 'actual_account_eligibility': 'UNVERIFIED',
            'margin_source': 'USER_SUPPLIED_HYPOTHETICAL_TOTALS_NOT_VENUE_VALIDATED',
            'settlement_currency': 'USD', 'rules': RULES}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, default=str, allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def write_report(directory, report):
    write_json(directory / 'report.json', report)
    page = ('<!doctype html><html lang="en"><meta charset="utf-8"><title>Kraken BTC/USD readiness</title>'
            '<style>body{font:17px system-ui;margin:32px;max-width:1200px}pre{white-space:pre-wrap;overflow-wrap:anywhere}</style>'
            '<h1>Kraken Multi-M: BTC/USD readiness</h1><p>Public market observations and explicitly hypothetical '
            'collateral scenarios. No account access or real orders. Retained PAPER fixture plans are not forward '
            'strategy evidence. BTC collateral is not spendable USD; USD reserves do not remove trading fees or funding.</p>'
            '<pre>' + html.escape(json.dumps(report, indent=2, ensure_ascii=False, default=str)) + '</pre></html>')
    (directory / 'report.html').write_text(page, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--environment', choices=HOSTS, default='live')
    parser.add_argument('--plans', type=Path, help='Exported CURRENT producer-input JSON; never an order submission')
    parser.add_argument('--configuration-fingerprint', help='Required persisted source risk fingerprint when observing plans')
    parser.add_argument('--collateral-scenario', type=Path, help='Explicit hypothetical BTC/USD cross-margin scenario JSON')
    parser.add_argument('--output', type=Path, help='Separate ignored local study directory, reusable for journal restart')
    args = parser.parse_args()
    if args.plans and not args.configuration_fingerprint:
        parser.error('--plans requires the persisted --configuration-fingerprint')
    directory = args.output or ROOT / 'storage/kraken_shadow' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    directory = directory.resolve()
    if not directory.is_relative_to((ROOT / 'storage/kraken_shadow').resolve()):
        parser.error('Keep this study under ignored storage/kraken_shadow')
    scenario = json.loads(args.collateral_scenario.read_text(encoding='utf-8')) if args.collateral_scenario else None
    capture = directory / 'raw' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    reader = KrakenPublicReader(args.environment, capture)
    snapshot = reader.snapshot()
    write_json(capture / 'snapshot.json', snapshot)
    report = {'scope': SCOPE, 'observed_at': snapshot['observed_at'], 'environment': args.environment,
              'market_count': len(snapshot['markets']), 'rules': RULES, 'plans': [], 'submitted': False,
              'snapshot_hash': digest(snapshot), 'raw_capture': str(capture),
              'forward_shadow_acceptance': False, 'account_eligibility': 'UNVERIFIED',
              'collateral_scenario': collateral_scenario(scenario, snapshot, time.time()) if scenario else None,
              'pending': ['Actual account/wallet eligibility and authoritative balances/margin',
                          'Profit-currency confirmation and USD-reserve policy',
                          'Available supported test environment before private lifecycle testing',
                          'Funding/cost accounting and aligned forward shadow']}
    if args.plans:
        source = json.loads(args.plans.read_text(encoding='utf-8'))
        write_json(capture / 'producer-plans.json', [entry['plan'] for entry in source])
        journal = ShadowJournal(directory, args.configuration_fingerprint, assessor=assess_kraken,
                                scope=SCOPE, source_identity={'environment': args.environment, 'schema': 1})
        class RetainedSnapshot:
            def snapshot(self):
                return snapshot
        try:
            for entry in source:
                plan = entry['plan']
                report['plans'].append(journal.observe(plan, RetainedSnapshot(), time.time()))
            report['plan_provenance'] = 'EXPORTED_PLAN_PUBLIC_OBSERVATION_NOT_FORWARD_ACCEPTANCE'
        finally:
            journal.close()
    write_report(directory, report)
    print('KRAKEN-SHADOW: public observation complete; private readiness remains pending')
    print('Report:', directory / 'report.html')


if __name__ == '__main__':
    main()
