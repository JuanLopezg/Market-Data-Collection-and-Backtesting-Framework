#!/usr/bin/env python3
"""How often the strategy's daily universe held an asset the venue cannot trade.

For every completed UTC day of the last ~13 months: the top N Binance USDT perpetuals by
the mean of the last 25 days' quote turnover (as the strategy's universe; full 25 days
required), then which of them Kraken (the symbol map in config/venues/kraken_futures)
or Hyperliquid (current listing, by base name) cannot trade, and which of those had a
7-day RSI above 50 and above 80 (the entry levels of the two PureRSI profiles).

Public data only. Approximations, stated in the output: today's Binance listings (coins
delisted since are absent) and today's venue listings (a contract added later counts as
available on every day); RSI above the entry level is a candidate, not a guaranteed
entry (the strategy also ranks and caps active signals).

Usage: python3 tools/universe_coverage_history.py --output storage/exchange_coverage/universe-history-<date>
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SYMBOL_MAP = ROOT / 'config/venues/kraken_futures/symbol_map.json'
SMA_DAYS = 25
RSI_LENGTH = 7
RSI_LEVELS = (50, 80)
HISTORY_DAYS = 400   # Binance klines limit per request: about 13 months


def get(url, body=None):
    request = urllib.request.Request(url, data=body, headers={'User-Agent': 'algoTrading-public-coverage',
                                                              'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.loads(response.read())


def binance_perpetuals():
    info = get('https://fapi.binance.com/fapi/v1/exchangeInfo')
    return [s['symbol'] for s in info['symbols'] if s['contractType'] == 'PERPETUAL' and s['quoteAsset'] == 'USDT'
            and s['status'] == 'TRADING' and s.get('underlyingType', 'COIN') == 'COIN']


def binance_daily(symbol):
    """Completed days only: {day: quote turnover} and {day: close}."""
    query = urllib.parse.urlencode({'symbol': symbol, 'interval': '1d', 'limit': HISTORY_DAYS})
    rows = get('https://fapi.binance.com/fapi/v1/klines?' + query)
    today = datetime.now(timezone.utc).strftime('%Y%m%d')
    turnover, closes = {}, {}
    for row in rows:
        day = datetime.fromtimestamp(row[0] / 1000, timezone.utc).strftime('%Y%m%d')
        if day != today:
            turnover[day], closes[day] = float(row[7]), float(row[4])
    return symbol, turnover, closes


def hyperliquid_bases():
    universe = get('https://api.hyperliquid.xyz/info', b'{"type":"meta"}')['universe']
    # "kPEPE" is 1000 PEPE; the asset is the same.
    return {name[1:] if name.startswith('k') and name[1:].isupper() else name
            for name in (row['name'] for row in universe if not row.get('isDelisted'))}


def binance_base(symbol):
    """BTCUSDT -> BTC, 1000PEPEUSDT -> PEPE; other digits are kept (1INCH)."""
    base = symbol[:-4]
    for prefix in ('1000000', '1000', '1M'):
        if base.startswith(prefix) and base[len(prefix):].isalpha():
            return base[len(prefix):]
    return base


def rsi(closes, length=RSI_LENGTH):
    """Wilder RSI of the last close; None without length + 1 closes."""
    if len(closes) < length + 1:
        return None
    changes = [b - a for a, b in zip(closes, closes[1:])]
    gain = sum(max(c, 0) for c in changes[:length]) / length
    loss = sum(max(-c, 0) for c in changes[:length]) / length
    for change in changes[length:]:
        gain = (gain * (length - 1) + max(change, 0)) / length
        loss = (loss * (length - 1) + max(-change, 0)) / length
    return 100.0 if loss == 0 else 100 - 100 / (1 + gain / loss)


def analyse(turnover, closes, venues, top_n):
    """Per day: the top_n universe and, per venue, the members it cannot trade.

    turnover/closes: {symbol: {day: value}}; venues: {name: function(symbol) -> listed}.
    """
    days = sorted({day for bars in turnover.values() for day in bars})
    result = []
    for index, day in enumerate(days):
        window = days[max(0, index - SMA_DAYS + 1):index + 1]
        if len(window) < SMA_DAYS:
            continue
        scores = []
        for symbol, bars in turnover.items():
            values = [bars[d] for d in window if d in bars]
            if len(values) == SMA_DAYS:
                scores.append((sum(values) / SMA_DAYS, symbol))
        universe = [symbol for _, symbol in sorted(scores, reverse=True)[:top_n]]
        row = {'day': day, 'universe': universe}
        for name, listed in venues.items():
            missing = [symbol for symbol in universe if not listed(symbol)]
            strength = {s: rsi([closes[s][d] for d in days[:index + 1] if d in closes[s]][-60:]) for s in missing}
            row[name] = {'missing': missing, **{
                'rsi_above_' + str(level): [s for s in missing if (strength[s] or 0) > level] for level in RSI_LEVELS}}
        result.append(row)
    return result


def summarize(days, venue):
    rows = [day[venue] for day in days]
    count = len(rows)
    summary = {'days_with_missing': sum(1 for row in rows if row['missing']),
               'average_missing_per_day': round(sum(len(row['missing']) for row in rows) / count, 3)}
    for level in RSI_LEVELS:
        key = 'rsi_above_' + str(level)
        summary['days_missing_' + key] = sum(1 for row in rows if row[key])
        summary['average_missing_' + key] = round(sum(len(row[key]) for row in rows) / count, 3)
    summary['last_30_days_with_missing'] = sum(1 for row in rows[-30:] if row['missing'])
    summary['most_missed'] = Counter(s for row in rows for s in row['missing']).most_common(15)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--top', type=int, default=20, help='universe size (the PureRSI profiles use 20)')
    args = parser.parse_args()
    symbols = binance_perpetuals()
    with ThreadPoolExecutor(8) as pool:
        fetched = list(pool.map(binance_daily, symbols))
    turnover = {symbol: bars for symbol, bars, _ in fetched}
    closes = {symbol: bars for symbol, _, bars in fetched}
    kraken = {row['source'] for row in json.loads(SYMBOL_MAP.read_text())['symbols']}
    hyperliquid = hyperliquid_bases()
    venues = {'kraken': lambda symbol: symbol in kraken,
              'hyperliquid': lambda symbol: binance_base(symbol) in hyperliquid}
    days = analyse(turnover, closes, venues, args.top)
    summary = {'retrieved_at': datetime.now(timezone.utc).isoformat(), 'binance_contracts': len(symbols),
               'universe_size': args.top, 'days': len(days), 'first_day': days[0]['day'], 'last_day': days[-1]['day'],
               'approximations': 'today\'s Binance and venue listings; RSI above the entry level is a candidate, '
                                 'not a guaranteed entry',
               **{venue: summarize(days, venue) for venue in venues}}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'result.json').write_text(json.dumps({'summary': summary, 'days': days}, indent=1, ensure_ascii=False))
    print(json.dumps(summary, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
