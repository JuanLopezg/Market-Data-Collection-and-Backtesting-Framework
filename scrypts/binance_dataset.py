from __future__ import annotations

import time
from collections import OrderedDict

import requests
import pandas as pd


# =========================
# CONFIG
# =========================
SYMBOLS_FILE = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/parquets/all_symbols.txt"

INTERVAL = "1d"
OUTPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/binance_{INTERVAL}_cc.parquet"


START_DATE = "2026-01-01"
END_DATE = None  # None = now

LIMIT = 1500
REQUEST_SLEEP_SECONDS = 0.12

QUOTE_ASSET = "USDT"
ONLY_TRADING = True
ONLY_PERPETUAL = True  # Change to False if you also want quarterly futures

BINANCE_FUTURES_EXCHANGE_INFO_URL = "https://fapi.binance.com/fapi/v1/exchangeInfo"
BINANCE_FUTURES_KLINES_URL = "https://fapi.binance.com/fapi/v1/klines"


# =========================
# LOAD SYMBOLS
# =========================
def normalize_symbol(token: str) -> str:
    token = token.strip().upper()

    # Allow inputs like BTC/USDT, BTC-USDT, BTC_USDT
    token = token.replace("/", "").replace("-", "").replace("_", "")

    # If your txt contains BTCUSDT, use BTC as lookup symbol
    for suffix in ("USDT", "USDC", "BUSD", "USD"):
        if token.endswith(suffix) and len(token) > len(suffix):
            token = token[: -len(suffix)]
            break

    return token


def load_symbols(symbols_path: str) -> list[str]:
    symbols = []
    seen = set()

    try:
        with open(symbols_path, "r", encoding="utf-8") as file:
            for line in file:
                for token in line.split(","):
                    symbol = normalize_symbol(token)

                    # Remove empty symbols and 1-character symbols
                    if len(symbol) <= 1:
                        continue

                    if symbol not in seen:
                        seen.add(symbol)
                        symbols.append(symbol)

    except FileNotFoundError:
        raise RuntimeError(f"Could not open symbols file: {symbols_path}")

    return symbols


# =========================
# BINANCE PAIR DISCOVERY
# =========================
def request_json(url: str, params: dict | None = None, max_retries: int = 5):
    for attempt in range(max_retries):
        try:
            response = requests.get(url, params=params, timeout=30)

            if response.status_code in (418, 429):
                retry_after = int(response.headers.get("Retry-After", "2"))
                print(f"Rate limited. Sleeping {retry_after} seconds...")
                time.sleep(retry_after)
                continue

            response.raise_for_status()
            return response.json()

        except requests.RequestException as e:
            if attempt == max_retries - 1:
                raise RuntimeError(f"Request failed after {max_retries} retries: {e}")

            sleep_seconds = 2 ** attempt
            print(f"Request error: {e}. Retrying in {sleep_seconds} seconds...")
            time.sleep(sleep_seconds)


def get_binance_futures_pairs() -> list[str]:
    data = request_json(BINANCE_FUTURES_EXCHANGE_INFO_URL)

    pairs = []

    for item in data.get("symbols", []):
        pair = item.get("symbol", "").upper()

        if not pair:
            continue

        if ONLY_TRADING and item.get("status") != "TRADING":
            continue

        if QUOTE_ASSET and item.get("quoteAsset") != QUOTE_ASSET:
            continue

        if ONLY_PERPETUAL and item.get("contractType") != "PERPETUAL":
            continue

        pairs.append(pair)

    return sorted(set(pairs))


def build_symbol_to_pairs_map(
    symbols: list[str],
    binance_pairs: list[str],
) -> tuple[dict[str, list[str]], list[str]]:
    symbol_to_pairs = OrderedDict()

    for symbol in symbols:
        matches = [pair for pair in binance_pairs if symbol in pair]
        symbol_to_pairs[symbol] = matches

    all_matched_pairs = sorted(
        {
            pair
            for matches in symbol_to_pairs.values()
            for pair in matches
        }
    )

    return symbol_to_pairs, all_matched_pairs


def print_symbol_matches(symbol_to_pairs: dict[str, list[str]]) -> None:
    print("\nBinance pairs containing each txt symbol:")
    print("========================================")

    total_symbols_with_matches = 0
    total_pair_mentions = 0

    for symbol, matches in symbol_to_pairs.items():
        if matches:
            total_symbols_with_matches += 1
            total_pair_mentions += len(matches)
            pairs_text = ", ".join(pair.lower() for pair in matches)
        else:
            pairs_text = "(no matches)"

        print(f"{symbol.lower()} -> {pairs_text}  [{len(matches)}]")

    unique_pairs = sorted(
        {
            pair
            for matches in symbol_to_pairs.values()
            for pair in matches
        }
    )

    print("========================================")
    print(f"Symbols from txt: {len(symbol_to_pairs):,}")
    print(f"Symbols with at least one Binance match: {total_symbols_with_matches:,}")
    print(f"Total pair mentions: {total_pair_mentions:,}")
    print(f"Unique Binance pairs matched: {len(unique_pairs):,}")


# =========================
# HELPERS
# =========================
def to_ms(date: str) -> int:
    return int(pd.Timestamp(date, tz="UTC").timestamp() * 1000)


def now_ms() -> int:
    return int(pd.Timestamp.now(tz="UTC").timestamp() * 1000)


def interval_to_ms(interval: str) -> int:
    interval_map = {
        "1m": 60_000,
        "3m": 3 * 60_000,
        "5m": 5 * 60_000,
        "15m": 15 * 60_000,
        "30m": 30 * 60_000,
        "1h": 60 * 60_000,
        "2h": 2 * 60 * 60_000,
        "4h": 4 * 60 * 60_000,
        "6h": 6 * 60 * 60_000,
        "8h": 8 * 60 * 60_000,
        "12h": 12 * 60 * 60_000,
        "1d": 24 * 60 * 60_000,
        "3d": 3 * 24 * 60 * 60_000,
        "1w": 7 * 24 * 60 * 60_000,
    }

    if interval not in interval_map:
        raise ValueError(f"Unsupported interval: {interval}")

    return interval_map[interval]


# =========================
# DOWNLOAD FUNCTION
# =========================
def download_futures_klines(pair: str) -> pd.DataFrame | None:
    print(f"\nDownloading {pair} futures {INTERVAL} data...")

    start_ms = to_ms(START_DATE)
    end_ms = now_ms() if END_DATE is None else to_ms(END_DATE)

    all_rows = []
    interval_ms = interval_to_ms(INTERVAL)

    while start_ms < end_ms:
        params = {
            "symbol": pair,
            "interval": INTERVAL,
            "startTime": start_ms,
            "endTime": end_ms,
            "limit": LIMIT,
        }

        try:
            response = requests.get(
                BINANCE_FUTURES_KLINES_URL,
                params=params,
                timeout=30,
            )

            if response.status_code in (400, 404):
                print(f"Skipping {pair}: pair not found or invalid on Binance Futures.")
                return None

            if response.status_code in (418, 429):
                retry_after = int(response.headers.get("Retry-After", "2"))
                print(f"{pair}: rate limited. Sleeping {retry_after} seconds...")
                time.sleep(retry_after)
                continue

            response.raise_for_status()
            rows = response.json()

        except requests.RequestException as e:
            print(f"Skipping {pair}: request error: {e}")
            return None

        if not rows:
            print(f"{pair}: no more rows returned.")
            break

        all_rows.extend(rows)

        last_open_time = rows[-1][0]
        next_start_ms = last_open_time + interval_ms

        if next_start_ms <= start_ms:
            print(f"Stopping {pair}: pagination did not advance.")
            break

        start_ms = next_start_ms

        print(f"{pair}: fetched {len(all_rows):,} rows so far...")

        time.sleep(REQUEST_SLEEP_SECONDS)

    if not all_rows:
        print(f"Skipping {pair}: no data downloaded.")
        return None

    df = pd.DataFrame(
        all_rows,
        columns=[
            "open_time",
            "open",
            "high",
            "low",
            "close",
            "volume",
            "close_time",
            "quote_volume",
            "trades",
            "taker_buy_base",
            "taker_buy_quote",
            "ignore",
        ],
    )

    df = df[["open_time", "open", "high", "low", "close", "volume"]].copy()

    df["ts"] = pd.to_datetime(df["open_time"], unit="ms", utc=True)

    df[["open", "high", "low", "close", "volume"]] = df[
        ["open", "high", "low", "close", "volume"]
    ].astype(float)

    df["symbol"] = pair

    df = df[["symbol", "ts", "open", "high", "low", "close", "volume"]]
    df = (
        df.sort_values("ts")
        .drop_duplicates(subset=["symbol", "ts"])
        .reset_index(drop=True)
    )

    print(f"{pair}: final rows {len(df):,}")

    return df


# =========================
# RUN
# =========================
symbols = load_symbols(SYMBOLS_FILE)

print("\nLoaded normalized txt symbols:")
print("==============================")
for symbol in symbols:
    print(symbol)
print("==============================")
print(f"Total unique txt symbols: {len(symbols):,}")

binance_pairs = get_binance_futures_pairs()

print("\nValid Binance Futures pairs found:")
print("==================================")
print(f"Total Binance pairs available: {len(binance_pairs):,}")
print("==================================")

symbol_to_pairs, pairs = build_symbol_to_pairs_map(symbols, binance_pairs)

print_symbol_matches(symbol_to_pairs)

print("\nUnique pairs that will be downloaded:")
print("=====================================")
for pair in pairs:
    print(pair)
print("=====================================")
print(f"Total unique pairs to download: {len(pairs):,}")

if not pairs:
    raise RuntimeError("No Binance pairs matched any symbols from the txt file.")

all_dfs = []

for pair in pairs:
    df_pair = download_futures_klines(pair)

    if df_pair is not None and not df_pair.empty:
        all_dfs.append(df_pair)

if not all_dfs:
    raise RuntimeError("No data was downloaded for any matched Binance pair.")

df_final = pd.concat(all_dfs, ignore_index=True)

df_final = df_final.sort_values(["symbol", "ts"]).reset_index(drop=True)
df_final = df_final.drop_duplicates(subset=["symbol", "ts"], keep="last")

print("\nPreview:")
print(df_final.head())
print(df_final.tail())

print("\nFinal dataset info:")
print("===================")
print(f"Symbols downloaded: {df_final['symbol'].nunique():,}")
print(f"Total rows: {len(df_final):,}")
print(df_final.dtypes)

print("\nSaving to parquet...")
df_final.to_parquet(OUTPUT_FILE, index=False)

print(f"Saved to: {OUTPUT_FILE}")