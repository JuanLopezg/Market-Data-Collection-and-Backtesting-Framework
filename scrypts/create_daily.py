import time
import requests
import pandas as pd
import plotly.graph_objects as go

# =========================
# CONFIG
# =========================
DATA_FILE = r"C:\Users\Juan\Documents\Python\algoTrading\ohlcv_fixed.csv"

BINANCE_URL = "https://api.binance.com/api/v3/klines"

TOKENS_TO_REPLACE = {
    "SHIB": "SHIBUSDT",
    "PEPE": "PEPEUSDT",
    "BONK": "BONKUSDT",
}

START_DATE = "2021-01-01"
END_DATE = "2025-10-16"

INTERVAL = "1d"
LIMIT = 1000


# =========================
# HELPERS
# =========================
def to_ms(date: str) -> int:
    return int(pd.Timestamp(date, tz="UTC").timestamp() * 1000)


def download_binance_klines(binance_symbol: str) -> pd.DataFrame:
    print(f"\nDownloading {binance_symbol} from Binance...")

    start_ms = to_ms(START_DATE)
    end_ms = to_ms(END_DATE) + (24 * 60 * 60 * 1000) - 1

    all_rows = []

    while start_ms <= end_ms:
        params = {
            "symbol": binance_symbol,
            "interval": INTERVAL,
            "startTime": start_ms,
            "endTime": end_ms,
            "limit": LIMIT,
        }

        response = requests.get(BINANCE_URL, params=params, timeout=30)
        response.raise_for_status()

        rows = response.json()

        if not rows:
            break

        all_rows.extend(rows)

        last_open_time = rows[-1][0]
        start_ms = last_open_time + 24 * 60 * 60 * 1000

        time.sleep(0.2)

    print(f"Downloaded rows for {binance_symbol}: {len(all_rows):,}")

    if not all_rows:
        raise ValueError(f"No Binance rows downloaded for {binance_symbol}")

    out = pd.DataFrame(
        all_rows,
        columns=[
            "open_time",
            "open",
            "high",
            "low",
            "close",
            "volume",
            "close_time",
            "quote_asset_volume",
            "number_of_trades",
            "taker_buy_base_volume",
            "taker_buy_quote_volume",
            "ignore",
        ],
    )

    out = out[["open_time", "open", "high", "low", "close", "volume"]].copy()

    out["ts"] = pd.to_datetime(out["open_time"], unit="ms", utc=True)

    out[["open", "high", "low", "close", "volume"]] = out[
        ["open", "high", "low", "close", "volume"]
    ].astype(float)

    out = out[["ts", "open", "high", "low", "close", "volume"]]
    out = out.sort_values("ts").reset_index(drop=True)

    return out


def create_replaced_token_df(df_original: pd.DataFrame, symbol: str, binance_symbol: str) -> pd.DataFrame:
    original_token = df_original[df_original["symbol"] == symbol].copy()

    if original_token.empty:
        raise ValueError(f"No original {symbol} rows found in df.")

    original_token["volume"] = pd.to_numeric(original_token["volume"], errors="coerce")

    cutoff_date = original_token.loc[original_token["volume"] != 0, "ts"].max()

    if pd.isna(cutoff_date):
        raise ValueError(f"No non-zero {symbol} volume found in original dataset.")

    print(f"\n{symbol} cutoff date: {cutoff_date}")

    binance_df = download_binance_klines(binance_symbol)
    binance_df["symbol"] = symbol

    token_final = binance_df[binance_df["ts"] <= cutoff_date].copy()

    original_volumes = original_token[["ts", "volume"]].copy()
    original_volumes = original_volumes.rename(columns={"volume": "original_volume"})

    token_final = token_final.merge(
        original_volumes,
        on="ts",
        how="left"
    )

    token_final["volume"] = token_final["original_volume"]
    token_final = token_final.drop(columns=["original_volume"])

    token_final = token_final[["ts", "symbol", "open", "high", "low", "close", "volume"]]
    token_final = token_final.sort_values("ts").reset_index(drop=True)

    missing_volumes = token_final["volume"].isna().sum()

    print(f"{symbol} final rows: {len(token_final):,}")
    print(f"{symbol} missing substituted volumes: {missing_volumes:,}")

    if missing_volumes > 0:
        print(f"WARNING: Some {symbol} rows did not find matching original volumes.")

    return token_final


# =========================
# LOAD CSV
# =========================
print("Loading CSV...")

df = pd.read_csv(DATA_FILE)

if "date" in df.columns and "ts" not in df.columns:
    df = df.rename(columns={"date": "ts"})

df["ts"] = pd.to_datetime(df["ts"], utc=True)

required_cols = ["ts", "symbol", "open", "high", "low", "close", "volume"]
df = df[required_cols].copy()

numeric_cols = ["open", "high", "low", "close", "volume"]
df[numeric_cols] = df[numeric_cols].apply(pd.to_numeric, errors="coerce")

df = df.sort_values(["symbol", "ts"]).reset_index(drop=True)

print("Loaded successfully!")
print(df.head())
print(df.dtypes)


# =========================
# MANUAL FIXES
# =========================
fixes = [
    ("SOL", "2023-06-12", {"high": 16, "low": 14.76}),
    ("SOL", "2023-12-30", {"high": 107.46}),
    ("SOL", "2023-12-31", {"high": 105.21}),
    ("SOL", "2024-01-01", {"high": 109.93}),
]

for symbol, date, values in fixes:
    mask = (df["symbol"] == symbol) & (df["ts"] == pd.Timestamp(date, tz="UTC"))

    if mask.sum() == 0:
        print(f"WARNING: no row found for {symbol} {date}")

    for col, val in values.items():
        df.loc[mask, col] = val


mask = (df["symbol"] == "XEC") & (df["ts"] == pd.Timestamp("2021-11-10", tz="UTC"))

if mask.sum() == 0:
    print("WARNING: no row found for XEC 2021-11-10")
else:
    df.loc[mask, "high"] = df.loc[mask, "open"]

print("Manual fixes applied.")


# =========================
# REMOVE UNWANTED SYMBOLS
# =========================
df = df[~df["symbol"].isin(["BTT", "NPXS"])].copy()


# =========================
# CREATE REPLACED TOKEN DATA
# =========================
replacement_dfs = []

for symbol, binance_symbol in TOKENS_TO_REPLACE.items():
    token_final = create_replaced_token_df(
        df_original=df,
        symbol=symbol,
        binance_symbol=binance_symbol
    )

    replacement_dfs.append(token_final)


df_replacements = pd.concat(replacement_dfs, ignore_index=True)


# =========================
# REPLACE TOKENS IN ORIGINAL DATASET
# =========================
symbols_to_replace = list(TOKENS_TO_REPLACE.keys())

db_final = df[~df["symbol"].isin(symbols_to_replace)].copy()

db_final = pd.concat([db_final, df_replacements], ignore_index=True)

db_final = db_final.sort_values(["symbol", "ts"]).reset_index(drop=True)
db_final = db_final.drop_duplicates(subset=["symbol", "ts"], keep="last")


# =========================
# FINAL CHECKS
# =========================
print("\n=========================")
print("FINAL CHECKS")
print("=========================")

print(f"Final rows: {len(db_final):,}")

for symbol in symbols_to_replace:
    print(f"{symbol} rows: {len(db_final[db_final['symbol'] == symbol]):,}")

duplicates = db_final.duplicated(subset=["symbol", "ts"], keep=False).sum()
print(f"Duplicate symbol+ts rows: {duplicates:,}")

for symbol in symbols_to_replace:
    print(f"\n{symbol} head:")
    print(db_final[db_final["symbol"] == symbol].head())

    print(f"\n{symbol} tail:")
    print(db_final[db_final["symbol"] == symbol].tail())


# =========================
# VOLUME CONTINUITY CHECK
# =========================
print("\n=========================")
print("VOLUME NON-ZERO PERIODS")
print("=========================")

for symbol in symbols_to_replace:
    data = db_final[db_final["symbol"] == symbol].copy().sort_values("ts")

    if data.empty:
        print(f"\n{symbol}: no data found")
        continue

    data["nonzero_volume"] = data["volume"] != 0
    data["block"] = (
        data["nonzero_volume"] != data["nonzero_volume"].shift()
    ).cumsum()

    periods = (
        data[data["nonzero_volume"]]
        .groupby("block")
        .agg(
            start=("ts", "min"),
            end=("ts", "max"),
            rows=("ts", "count")
        )
        .reset_index(drop=True)
    )

    print(f"\n====================")
    print(symbol)
    print("====================")

    if periods.empty:
        print("No periods with volume != 0")
    else:
        print(periods)


# =========================
# GAP CHECK
# =========================
print("\n=========================")
print("CHECK GAPS 1D")
print("=========================")

symbols_with_gaps = []

for symbol, group in db_final.groupby("symbol"):
    group = group.sort_values("ts")
    diffs = group["ts"].diff().dropna()
    gaps = diffs[diffs != pd.Timedelta(days=1)]

    if not gaps.empty:
        symbols_with_gaps.append(symbol)

print(f"Symbols with gaps: {len(symbols_with_gaps)}")

if symbols_with_gaps:
    print(", ".join(sorted(symbols_with_gaps)))
else:
    print("No gaps 🎉")

# =========================
# SAVE PARQUET (1D OHLC)
# =========================
PARQUET_FILE = r"C:\Users\Juan\Documents\Python\algoTrading\ohlc_1d.parquet"

db_final.to_parquet(PARQUET_FILE, index=False)

print("\nSaved Parquet file to:")
print(PARQUET_FILE)