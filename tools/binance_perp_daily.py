#!/usr/bin/env python3
"""Download completed daily candles of Binance USDT-margined perpetuals into the research CSV.

Output columns are the ones lib/src/utils/database_utils.cpp reads:
    date,symbol,open,high,low,close,volume
with `volume` = QUOTE volume in USDT (the kline "quote asset volume"), so the PureRSI
liquidity universe (top 20 by SMA(volume, 25)) ranks coins by dollar turnover, as the
PAPER chain does. Base-asset volume is not comparable across coins (1000PEPE vs BTC).

Universe: every PERPETUAL contract with quote asset USDT and underlying type COIN listed
by /fapi/v1/exchangeInfo, whatever its status, so contracts Binance still lists after a
delisting keep their history. Contracts already removed from exchangeInfo are missing
(survivorship bias). Only completed UTC days are written (the current day is skipped).

Public endpoints only; no credentials.

Usage:
    python3 tools/binance_perp_daily.py --start 2024-12-07 --output storage/databases/1d_binance_perp_quote.csv
"""

import argparse
import csv
import datetime as dt
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

EXCHANGE_INFO_URL = "https://fapi.binance.com/fapi/v1/exchangeInfo"
KLINES_URL = "https://fapi.binance.com/fapi/v1/klines"
DAY_MS = 86_400_000
KLINES_LIMIT = 1000          # weight 5 per request
REQUEST_PAUSE_SECONDS = 0.15  # stays well below the 2,400 weight/minute limit


def get_json(url: str, params: dict | None = None, attempts: int = 5):
    if params:
        url = url + "?" + urllib.parse.urlencode(params)
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code in (418, 429) or error.code >= 500:
                wait = int(error.headers.get("Retry-After", "0") or 0) or 5 * attempt
                print(f"HTTP {error.code} for {url}; retry in {wait}s", file=sys.stderr)
                time.sleep(wait)
                continue
            raise
        except urllib.error.URLError as error:
            print(f"Network error {error.reason}; retry {attempt}/{attempts}", file=sys.stderr)
            time.sleep(3 * attempt)
    raise RuntimeError(f"Giving up on {url}")


def perpetual_symbols() -> list[dict]:
    info = get_json(EXCHANGE_INFO_URL)
    symbols = [
        s for s in info["symbols"]
        if s.get("contractType") == "PERPETUAL"
        and s.get("quoteAsset") == "USDT"
        and s.get("underlyingType", "COIN") == "COIN"
    ]
    return sorted(symbols, key=lambda s: s["symbol"])


def daily_klines(symbol: str, start_ms: int, end_ms: int) -> list[list]:
    rows = []
    cursor = start_ms
    while cursor <= end_ms:
        try:
            batch = get_json(KLINES_URL, {
                "symbol": symbol, "interval": "1d", "startTime": cursor,
                "endTime": end_ms, "limit": KLINES_LIMIT,
            })
        except urllib.error.HTTPError as error:
            if error.code != 400:
                raise
            # Binance refuses candles for some listed contracts (not yet trading, settled).
            print(f"Skipping {symbol}: HTTP 400 {error.read().decode(errors='replace')[:120]}", file=sys.stderr)
            return rows
        time.sleep(REQUEST_PAUSE_SECONDS)
        if not batch:
            break
        rows.extend(batch)
        cursor = batch[-1][0] + DAY_MS
        if len(batch) < KLINES_LIMIT:
            break
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", required=True, help="first UTC day, YYYY-MM-DD")
    parser.add_argument("--end", help="last UTC day, YYYY-MM-DD (default: yesterday, the last completed day)")
    parser.add_argument("--output", required=True, help="CSV file to write")
    args = parser.parse_args()

    today = dt.datetime.now(dt.timezone.utc).date()
    start = dt.date.fromisoformat(args.start)
    end = dt.date.fromisoformat(args.end) if args.end else today - dt.timedelta(days=1)
    if end >= today:
        end = today - dt.timedelta(days=1)  # never write the day still in progress
    if start > end:
        parser.error("start must be on or before the last completed day")

    start_ms = int(dt.datetime(start.year, start.month, start.day, tzinfo=dt.timezone.utc).timestamp() * 1000)
    end_ms = int(dt.datetime(end.year, end.month, end.day, tzinfo=dt.timezone.utc).timestamp() * 1000)

    symbols = perpetual_symbols()
    statuses = {}
    for s in symbols:
        statuses[s["status"]] = statuses.get(s["status"], 0) + 1
    print(f"{len(symbols)} USDT perpetuals in exchangeInfo {statuses}; days {start}..{end}")

    written = 0
    with_data = 0
    with open(args.output, "w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["date", "symbol", "open", "high", "low", "close", "volume"])
        for index, s in enumerate(symbols, 1):
            rows = daily_klines(s["symbol"], start_ms, end_ms)
            kept = 0
            for k in rows:
                open_time = int(k[0])
                if open_time < start_ms or open_time > end_ms:
                    continue
                day = dt.datetime.fromtimestamp(open_time / 1000, dt.timezone.utc).date().isoformat()
                # kline: [open time, open, high, low, close, base volume, close time, quote volume, ...]
                writer.writerow([day, s["symbol"], k[1], k[2], k[3], k[4], k[7]])
                kept += 1
            written += kept
            with_data += 1 if kept else 0
            if index % 50 == 0 or index == len(symbols):
                print(f"{index}/{len(symbols)} symbols, {written} rows")
    print(f"Wrote {written} rows for {with_data} symbols with data to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
