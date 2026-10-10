#!/usr/bin/env python3
"""Reproducible public perpetual coverage against Binance 25-day quote turnover."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import hashlib
import html
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
# Explicit contract-unit aliases only; never strip arbitrary digits (e.g. 1INCH).
ALIASES = {'XBT': 'BTC', '1000PEPE': 'PEPE', 'kPEPE': 'PEPE', '1000SHIB': 'SHIB',
           'kSHIB': 'SHIB', '1000BONK': 'BONK', 'kBONK': 'BONK', '1000FLOKI': 'FLOKI',
           'kFLOKI': 'FLOKI', '1000LUNC': 'LUNC', 'kLUNC': 'LUNC', '1000DOGS': 'DOGS',
           'kDOGS': 'DOGS', '1000SATS': 'SATS', '1000CHEEMS': 'CHEEMS', '1000000MOG': 'MOG'}


def asset(name):
    return ALIASES.get(name, name)


def fetch(url, output, body=None):
    request = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None,
                                     headers={'Content-Type': 'application/json', 'User-Agent': 'algoTrading-public-coverage'})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=25) as response:
                raw = response.read()
                data = json.loads(raw)
                envelope = {'url': url, 'request': body, 'retrieved_at': datetime.now(timezone.utc).isoformat(),
                            'http_date': response.headers.get('Date'), 'sha256': hashlib.sha256(raw).hexdigest(), 'data': data}
                output.write_text(json.dumps(envelope, ensure_ascii=False), encoding='utf-8')
                return data
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == 2:
                raise
            time.sleep(min(60, max(1, int(error.headers.get('Retry-After', '10')))))
        except (urllib.error.URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(2 ** attempt)


def turnover(rows, start, end, onboard):
    expected = set(range(start, end, 86400000))
    seen, total = set(), Decimal(0)
    for row in rows:
        stamp = int(row[0])
        value = Decimal(str(row[7]))  # Actual quote asset volume, not base volume * close.
        if stamp not in expected or stamp in seen or int(row[6]) >= end or not value.is_finite() or value < 0:
            raise ValueError('Invalid/duplicate/uncompleted Binance daily quote-volume candle')
        seen.add(stamp)
        total += value
    missing = expected - seen
    # New listings contribute zero before first full daily candle. An unexplained
    # gap after listing is an incomplete study, never fabricated zero volume.
    if any(day >= ((int(onboard) + 86399999) // 86400000) * 86400000 for day in missing):
        raise ValueError('Missing quote-volume candle after listing')
    return total, len(seen)


def kraken_catalog(instruments, tickers):
    quotes = {row['symbol']: row for row in tickers['tickers']}
    result = {}
    for row in instruments['instruments']:
        ticker = quotes.get(row['symbol'], {})
        if (row.get('tradeable') is True and row.get('isExpired') is False and not row.get('tradfi', False)
                and ticker.get('tag') == 'perpetual' and ticker.get('suspended') is False
                and Decimal(str(ticker.get('markPrice', 0))) > 0):
            base = row.get('base') or ticker['pair'].split(':')[0]
            result.setdefault(asset(base), []).append(row['symbol'])
    return result


def hyperliquid_catalog(response):
    meta, contexts = response
    if len(meta['universe']) != len(contexts):
        raise ValueError('Hyperliquid metadata/context index mismatch')
    result = {}
    for market, context in zip(meta['universe'], contexts):
        if (not market.get('isDelisted', False) and context.get('midPx') is not None
                and Decimal(str(context['midPx'])) > 0 and Decimal(str(context.get('markPx', 0))) > 0):
            # HIP-3 namespaces are retained in evidence; matching is by the local
            # asset label only, with full matched contract names exposed for review.
            base = market['name'].split(':', 1)[-1]
            result.setdefault(asset(base), []).append(market['name'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--days', type=int, default=25)
    parser.add_argument('--top', type=int, default=50)
    args = parser.parse_args()
    if not 1 <= args.days <= 100 or not 1 <= args.top <= 100:
        parser.error('Use 1..100 completed days and 1..100 assets')
    directory = ROOT / 'storage/exchange_coverage' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    raw = directory / 'raw'
    raw.mkdir(parents=True)
    server = fetch('https://fapi.binance.com/fapi/v1/time', raw / 'binance-time.json')
    today = datetime.fromtimestamp(server['serverTime'] / 1000, timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)
    start, end = int((today - timedelta(days=args.days)).timestamp() * 1000), int(today.timestamp() * 1000)
    exchange = fetch('https://fapi.binance.com/fapi/v1/exchangeInfo', raw / 'binance-markets.json')
    candidates = [row for row in exchange['symbols'] if row['status'] == 'TRADING'
                  and row['contractType'] == 'PERPETUAL' and row['quoteAsset'] == 'USDT'
                  and row.get('underlyingType') == 'COIN']
    print(f'Fetching {len(candidates)} active Binance crypto perpetuals; {today.date()} UTC excluded', flush=True)

    def history(row):
        parameters = urllib.parse.urlencode({'symbol': row['symbol'], 'interval': '1d', 'startTime': start,
                                            'endTime': end - 1, 'limit': args.days + 1})
        bars = fetch('https://fapi.binance.com/fapi/v1/klines?' + parameters, raw / (row['symbol'] + '.json'))
        total, days = turnover(bars, start, end, row['onboardDate'])
        time.sleep(.12)
        return {'asset': asset(row['baseAsset']), 'binance': row['symbol'],
                'quote_volume_usdt': str(total), 'sma_quote_volume_usdt': str(total / args.days), 'observed_days': days}

    with ThreadPoolExecutor(max_workers=4) as workers:
        universe = list(workers.map(history, candidates))
    universe.sort(key=lambda row: (-Decimal(row['quote_volume_usdt']), row['binance']))
    # Deduplicate base assets explicitly rather than let two unit contracts occupy
    # two top-50 coin slots. Retain the larger single USDT-contract turnover.
    selected, seen = [], set()
    for row in universe:
        if row['asset'] not in seen:
            selected.append(row)
            seen.add(row['asset'])
        if len(selected) == args.top:
            break
    if len(selected) != args.top:
        raise ValueError('Insufficient complete Binance reference assets')
    print('Binance ranking complete; fetching current venue catalogues', flush=True)
    instruments = fetch('https://futures.kraken.com/derivatives/api/v3/instruments', raw / 'kraken-instruments.json')
    tickers = fetch('https://futures.kraken.com/derivatives/api/v3/tickers', raw / 'kraken-tickers.json')
    kraken = kraken_catalog(instruments, tickers)
    main_response = fetch('https://api.hyperliquid.xyz/info', raw / 'hyperliquid-main.json', {'type': 'metaAndAssetCtxs'})
    hyper_main = hyperliquid_catalog(main_response)
    dexes = fetch('https://api.hyperliquid.xyz/info', raw / 'hyperliquid-dexes.json', {'type': 'perpDexs'})
    hyper_builder = {}
    for dex in dexes:
        if not dex or not dex.get('name'):
            continue
        response = fetch('https://api.hyperliquid.xyz/info', raw / ('hyperliquid-' + dex['name'] + '.json'),
                         {'type': 'metaAndAssetCtxs', 'dex': dex['name']})
        for key, names in hyperliquid_catalog(response).items():
            hyper_builder.setdefault(key, []).extend(names)
    for rank, row in enumerate(selected, 1):
        row.update(rank=rank, kraken=kraken.get(row['asset'], []), hyperliquid_main=hyper_main.get(row['asset'], []),
                   hyperliquid_hip3=hyper_builder.get(row['asset'], []))
    total_volume = sum(Decimal(row['quote_volume_usdt']) for row in selected)
    summary = {}
    for venue, fields in {'Binance': ['binance'], 'Kraken': ['kraken'], 'Hyperliquid main': ['hyperliquid_main'],
                          'Hyperliquid including HIP-3': ['hyperliquid_main', 'hyperliquid_hip3']}.items():
        covered = [row for row in selected if any(row[field] for field in fields)]
        summary[venue] = {'count': len(covered), 'percentage': 100 * len(covered) / args.top,
            'reference_volume_percentage': float(100 * sum(Decimal(row['quote_volume_usdt']) for row in covered) / total_volume),
            'missing': [row['asset'] for row in selected if row not in covered]}
    result = {'retrieved_at': datetime.now(timezone.utc).isoformat(), 'window_start_utc': datetime.fromtimestamp(start/1000,timezone.utc).isoformat(),
              'window_end_exclusive_utc': today.isoformat(), 'candidate_contracts': len(candidates),
              'method': 'Current active Binance COIN USDT perpetuals; sum actual 1d quote volume over completed UTC days; not spot/all Binance',
              'aliases': ALIASES, 'summary': summary, 'assets': selected, 'all_candidates': universe,
              'limitations': ['Public coverage is not user account/jurisdiction eligibility',
                              'Scaled contracts need explicit quantity/price conversion before execution',
                              'HIP-3 issuer namespaces require separate operational/collateral assessment',
                              'Current listed universe has survivorship bias; not historical point-in-time selection',
                              'No account/private endpoint, liquidity/cost ranking or collateral verification in this script']}
    (directory / 'comparison.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    with (directory / 'coverage.csv').open('w', newline='', encoding='utf-8') as output:
        writer = csv.DictWriter(output, fieldnames=list(selected[0]))
        writer.writeheader()
        writer.writerows({key: ';'.join(value) if isinstance(value, list) else value for key,value in row.items()} for row in selected)
    table = ''.join('<tr>' + ''.join('<td>' + html.escape(str(row[key])) + '</td>' for key in
                   ('rank','asset','binance','sma_quote_volume_usdt','kraken','hyperliquid_main','hyperliquid_hip3')) + '</tr>' for row in selected)
    page = '<!doctype html><meta charset="utf-8"><title>Perpetual coverage</title><style>body{font:17px system-ui;margin:30px}td,th{padding:9px;border:1px solid #aaa}table{border-collapse:collapse}pre{white-space:pre-wrap}</style>'
    page += '<h1>Current public perpetual coverage</h1><p>Binance crypto USDT perpetual reference. Actual quote turnover, completed UTC days.</p><pre>'
    page += html.escape(json.dumps({key: result[key] for key in ('retrieved_at','window_start_utc','window_end_exclusive_utc','summary','limitations')},indent=2))
    page += '</pre><table><tr><th>Rank</th><th>Asset</th><th>Binance</th><th>Average daily USDT</th><th>Kraken</th><th>Hyperliquid main</th><th>HIP-3</th></tr>' + table + '</table>'
    (directory / 'report.html').write_text(page, encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)
    print('Evidence:', directory, flush=True)


if __name__ == '__main__':
    main()
