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
                 'Balance: ' + dollars(snapshot['balance']), 'Equity: ' + dollars(snapshot['equity']),
                 'Active positions (USD exposure):']
        lines.extend(snapshot['positions'] or ['None'])
        if previous:
            prior = json.loads(previous[0])
            clock = datetime.fromisoformat(prior['observed_at']).strftime('%Y-%m-%d %H:%M UTC')
            lines.append("Yesterday's positions (last snapshot " + clock + '):')
            lines.extend(prior['positions'] or ['None'])
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
