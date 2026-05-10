import pandas as pd
import plotly.graph_objects as go


DIVISOR = 12  # ← change this to 4, 6, 8, 12, etc.

if 24 % DIVISOR != 0:
    raise ValueError("DIVISOR must divide 24 exactly, e.g. 1, 2, 3, 4, 6, 8, 12, 24")


# =========================
# CONFIG
# =========================
DATA_FILE = "/mnt/c/Users/Juan/Documents/Python/algoTrading/1h_binance.parquet"
OUTPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/{DIVISOR}h_binance.parquet"


# =========================
# LOAD DATA
# =========================
print("Loading cleaned dataset...")

df = pd.read_parquet(
    DATA_FILE,
    engine="pyarrow"
)

print("Loaded successfully!")

df = df.sort_values(["symbol", "ts"]).copy()

# =========================
# CREATE AGGREGATED DATAFRAME
# =========================
df["ts_group"] = df["ts"].dt.floor(f"{DIVISOR}h")

df_agg = (
    df.groupby(["symbol", "ts_group"])
    .agg(
        open=("open", "first"),
        high=("high", "max"),
        low=("low", "min"),
        close=("close", "last"),
        volume=("volume", "sum"),
        count=("open", "count")
    )
    .reset_index()
)

# Keep only full candles
df_agg = df_agg[df_agg["count"] == DIVISOR].copy()

df_agg = df_agg.drop(columns=["count"])
df_agg = df_agg.rename(columns={"ts_group": "ts"})
df_agg = df_agg.sort_values(["symbol", "ts"]).reset_index(drop=True)

print(df_agg.head())
print(df_agg.dtypes)
print(f"Original rows: {len(df):,}")
print(f"{DIVISOR}h rows: {len(df_agg):,}")

# =========================
# FILTER SYMBOLS WITH >= 100 ROWS
# =========================
symbol_counts = df["symbol"].value_counts()

valid_symbols = symbol_counts[symbol_counts >= 100].index

df = df[df["symbol"].isin(valid_symbols)].copy()

print(f"Symbols kept (>=100 rows): {len(valid_symbols)}")
print(f"Rows after filtering: {len(df):,}")


df_agg.to_parquet(
    OUTPUT_FILE,
    engine="pyarrow",
    compression="snappy",
    index=False
)

print(f"\nSaved aggregated dataset to: {OUTPUT_FILE}")