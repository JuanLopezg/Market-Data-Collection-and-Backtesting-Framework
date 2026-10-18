"""USD portfolio observations and one daily summary from the read-only account."""
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import json

from kraken_shadow import SCOPE, fresh
from shadow_observer import number


def dollars(value):
    amount = number(value)
    return ('-$' if amount < 0 else '$') + format(abs(amount), ',.2f')


def position_values(result, market, now):
    """Use fresh verified linear USD contract marks, never average entry prices."""
    quotes = {}
    try:
        if market['scope'] != SCOPE or market['environment'] != result['environment'] or not market['complete']:
            raise ValueError('Wrong portfolio price source')
        fresh(market['observed_at'], now)
        for clock in market['server_times']:
            fresh(clock, now)
        quotes = {row['symbol']: row for row in market['markets'].values()}
    except (ValueError, KeyError, TypeError):
        pass
    values = []
    for position in sorted(result['positions'], key=lambda row: row['symbol']):
        symbol = position['symbol']
        label = symbol.removeprefix('PF_').removesuffix('USD').replace('XBT', 'BTC')
        value = None
        quote = quotes.get(symbol)
        if symbol.startswith('PF_') and quote and quote.get('trading') and quote.get('quote_currency') == 'USD':
            try:
                mark = number(quote['mark_price'])
                if mark <= 0:
                    raise ValueError('Invalid mark')
                value = str(number(position['size']) * mark)
            except (ValueError, KeyError, TypeError, ArithmeticError):
                pass
        values.append({'asset': label, 'side': position['side'].upper(), 'notional_usd': value})
    return values


def collateral_risk(result, market, now):
    """BTC collateral and the price drops that would use up the maintenance-margin headroom.

    Reporting only (no stop or order). Linear estimate: a drop of d reduces the BTC
    collateral by d x its post-haircut value and a net long position by d x its notional;
    maintenance margin is taken as fixed, so the real distance is slightly larger.
    """
    totals = result['venue_totals_usd']
    headroom = number(totals['marginEquity']) - number(totals['maintenanceMargin'])
    btc = result['currencies'].get('BTC')
    btc_quantity = number(btc['quantity']) if btc else Decimal(0)
    btc_collateral = number(btc['collateral']) if btc else Decimal(0)
    btc_value = number(btc['value']) if btc else Decimal(0)
    values = position_values(result, market, now)
    net_long = None
    if all(row['notional_usd'] is not None for row in values):
        net_long = sum((number(row['notional_usd']) * (1 if row['side'] == 'LONG' else -1) for row in values), Decimal(0))

    def drop(exposure):
        # None: unknown; 'NONE': a fall does not reduce the margin (no long exposure).
        if exposure is None:
            return None
        if exposure <= 0:
            return 'NONE'
        return str(max(Decimal(0), headroom) / exposure)

    return {
        'btc_quantity': str(btc_quantity), 'btc_value_usd': str(btc_value),
        'btc_collateral_usd': str(btc_collateral),
        'btc_haircut': str(1 - btc_collateral / btc_value) if btc_value > 0 else None,
        'margin_equity_usd': totals['marginEquity'], 'maintenance_margin_usd': totals['maintenanceMargin'],
        'maintenance_headroom_usd': str(headroom),
        'net_long_usd': str(net_long) if net_long is not None else None,
        'btc_drop_to_maintenance': drop(btc_collateral),
        'crypto_drop_to_maintenance': drop(None if net_long is None else btc_collateral + net_long),
    }


def percent(fraction):
    if fraction is None:
        return 'unavailable'
    if fraction == 'NONE':
        return 'no long exposure'
    value = number(fraction)
    return 'more than 100%' if value >= 1 else format(value * 100, '.1f') + '%'


def collateral_lines(risk):
    lines = []
    if number(risk['btc_quantity']) != 0:
        lines.append('BTC collateral: ' + format(number(risk['btc_quantity']), 'f') + ' BTC = ' +
                     dollars(risk['btc_value_usd']) + ' (' + dollars(risk['btc_collateral_usd']) + ' after haircut)')
    lines.append('Maintenance margin: ' + dollars(risk['maintenance_margin_usd']) +
                 ', headroom ' + dollars(risk['maintenance_headroom_usd']))
    lines.append('Drop to maintenance margin: BTC alone ' + percent(risk['btc_drop_to_maintenance']) +
                 ', BTC and positions together ' + percent(risk['crypto_drop_to_maintenance']))
    return lines


def position_lines(result, market, now):
    equity = number(result['venue_totals_usd']['marginEquity'])
    lines = []
    for row in position_values(result, market, now):
        value = 'USD value unavailable'
        if row['notional_usd'] is not None:
            notional = number(row['notional_usd'])
            share = format(notional / equity * 100, '.2f') + '% equity' if equity > 0 else 'equity % unavailable'
            value = dollars(notional) + ' (' + share + ')'
        lines.append(row['asset'] + ' ' + row['side'] + ': ' + value)
    return lines


def position_table(lines):
    """Render persisted position lines too, so yesterday survives an upgrade unchanged."""
    if not lines:
        return ['None']
    rows = [('Asset', 'Side', 'USD', '% Eq')]
    for line in lines:
        heading, separator, value = line.partition(': ')
        asset, space, side = heading.rpartition(' ')
        if not separator or not space or side not in ('LONG', 'SHORT'):
            # Never invent columns from an unfamiliar historical snapshot.
            return lines
        exposure, _, share = value.partition(' (')
        exposure = 'N/A' if exposure == 'USD value unavailable' else exposure
        share = share.removesuffix(' equity)') if share.endswith('% equity)') else 'N/A'
        rows.append((asset, side, exposure, share))
    widths = [max(len(row[column]) for row in rows) for column in range(4)]
    def formatted(row):
        return '  '.join(value.ljust(widths[column]) if column < 2 else value.rjust(widths[column])
                         for column, value in enumerate(row))
    return [formatted(rows[0]), '  '.join('-' * width for width in widths),
            *(formatted(row) for row in rows[1:])]


class DailyPortfolio:
    """Persist each UTC day's latest observation; never invent yesterday's holdings."""

    def __init__(self, db, account_name, hour_utc):
        self.db, self.account_name, self.hour_utc = db, account_name, hour_utc
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS portfolio_days (day TEXT PRIMARY KEY, payload TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS portfolio_summary_days (day TEXT PRIMARY KEY);
        ''')

    def yesterday(self, observed_at):
        date = datetime.fromtimestamp(observed_at, timezone.utc).date() - timedelta(days=1)
        row = self.db.execute('SELECT payload FROM portfolio_days WHERE day=?', (date.isoformat(),)).fetchone()
        return json.loads(row[0]) if row else None

    def record(self, result, market, now):
        timestamp = datetime.fromtimestamp(result['observed_at'], timezone.utc)
        day = timestamp.date().isoformat()
        snapshot = {'observed_at': timestamp.isoformat(),
            'balance': str(sum((number(row['value']) for row in result['currencies'].values()), Decimal(0))),
            'equity': result['venue_totals_usd']['marginEquity'],
            'positions': position_lines(result, market, now)}
        self.db.execute('INSERT OR REPLACE INTO portfolio_days VALUES (?,?)', (day, json.dumps(snapshot)))
        emitted = self.db.execute('SELECT 1 FROM portfolio_summary_days WHERE day=?', (day,)).fetchone()
        if self.hour_utc is None or emitted or timestamp.hour < self.hour_utc:
            return None
        yesterday = (timestamp.date() - timedelta(days=1)).isoformat()
        previous = self.db.execute('SELECT payload FROM portfolio_days WHERE day=?', (yesterday,)).fetchone()
        lines = ['As of: ' + timestamp.strftime('%Y-%m-%d %H:%M UTC'),
                 'Balance: ' + dollars(snapshot['balance']), 'Equity: ' + dollars(snapshot['equity'])]
        lines.extend(collateral_lines(collateral_risk(result, market, now)))
        lines.append('Active positions (USD exposure):')
        lines.extend(position_table(snapshot['positions']))
        if previous:
            prior = json.loads(previous[0])
            clock = datetime.fromisoformat(prior['observed_at']).strftime('%Y-%m-%d %H:%M UTC')
            lines.append("Yesterday's positions (last snapshot " + clock + '):')
            lines.extend(position_table(prior['positions']))
        else:
            lines.append("Yesterday's positions: unavailable (no snapshot)")
        identity = 'kraken-account:' + result['account_identity_hash'] + ':DAILY_PORTFOLIO'
        exists = self.db.execute('SELECT 1 FROM portfolio_summary_days LIMIT 1').fetchone()
        event = {'eventId': identity + ':' + day, 'recordedAt': timestamp.isoformat(),
            'transition': 'UPDATED' if exists else 'OPENED', 'alert': {'id': identity,
                'timestamp': timestamp.isoformat(), 'severity': 'INFO', 'status': 'ACTIVE',
                'account': self.account_name, 'service': 'KrakenAccountReadOnly',
                'eventType': 'DAILY_PORTFOLIO', 'title': 'Daily portfolio', 'detail': '\n'.join(lines)}}
        self.db.execute('INSERT INTO portfolio_summary_days VALUES (?)', (day,))
        return event
