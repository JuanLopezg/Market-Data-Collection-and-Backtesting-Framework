import pandas as pd
import yfinance as yf


TICKERS = [
    "AAPL",
    "NVDA",
    "MSFT",
    "AMZN",
    "GOOGL",
    "GOOG",
    "AVGO",
    "SPCX",
    "META",
    "TSLA",
    "MU",
    "WMT",
    "AMD",
    "ASML",
    "CSCO",
    "INTC",
    "COST",
    "AMAT",
    "LRCX",
    "NFLX",
    "PLTR",
    "ARM",
    "PANW",
    "TXN",
    "LIN",
    "KLAC",
    "AMGN",
    "TMUS",
    "PEP",
    "CRWD",

    # Benchmark
    "^NDX",
]
START_DATE = "2020-01-01"
END_DATE = None

OUTPUT_FILE = "/mnt/c/Users/Juan/Documents/Python/algoTrading/stocks_dailyC.parquet"

all_data = []

for ticker in TICKERS:
    print(f"Downloading {ticker} data...")

    df = yf.download(
        ticker,
        start=START_DATE,
        end=END_DATE,
        interval="1d",
        auto_adjust=False,
        progress=False,
        multi_level_index=False
    )

    if df.empty:
        print(f"No data for {ticker}")
        continue

    df = df.reset_index()

    df = df.rename(columns={
        "Date": "ts",
        "Open": "open",
        "High": "high",
        "Low": "low",
        "Close": "close",
        "Adj Close": "adj_close",
        "Volume": "volume"
    })

    df["ts"] = pd.to_datetime(df["ts"], utc=True)

    df = df.drop(columns=["adj_close"], errors="ignore")

    df["symbol"] = ticker

    df = df[["symbol", "ts", "open", "high", "low", "close", "volume"]]

    all_data.append(df)

if not all_data:
    raise ValueError("No stock data was downloaded.")

# =========================
# COMBINE ALL DATA
# =========================
final_df = pd.concat(all_data, ignore_index=True)

# =========================
# ALIGN ALL STOCKS TO SAME START DATE
# =========================
first_dates = final_df.groupby("symbol")["ts"].min()

print("\nFirst available date per stock:")
print(first_dates)

common_start_date = first_dates.max()

print(f"\nCommon start date: {common_start_date}")

final_df = final_df[final_df["ts"] >= common_start_date].copy()

# Optional: sort neatly
final_df = final_df.sort_values(["ts", "symbol"]).reset_index(drop=True)

print("Download complete!\n")

# =========================
# BASIC CHECKS
# =========================
print("Head:")
print(final_df.head())

print("\nTail:")
print(final_df.tail())

print("\nDtypes:")
print(final_df.dtypes)

print(f"\nTotal rows: {len(final_df):,}")

print("\nUnique symbols:")
print(final_df["symbol"].unique())

# =========================
# CHECK FOR NaNs
# =========================
print("\nNaN values per column:")
print(final_df.isna().sum())

print("\nAny NaNs in dataset?")
print(final_df.isna().any().any())

# =========================
# SAVE FILE
# =========================
final_df.to_parquet(
    OUTPUT_FILE,
    engine="pyarrow",
    compression="snappy",
    index=False
)

print(f"\nSaved file to: {OUTPUT_FILE}")