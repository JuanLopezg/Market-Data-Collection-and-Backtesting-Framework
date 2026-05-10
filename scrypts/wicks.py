import pandas as pd
import numpy as np

# =========================
# CONFIG
# =========================
INPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/1d_ohlcv.parquet"
OUTPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/1d_ohlcv_wicks.parquet"

ROWS_PER_GROUP = 150
RANDOM_SEED = 42


# =========================
# LOAD DATABASE
# =========================
print("Loading parquet...")

df = pd.read_parquet(INPUT_FILE)

df["ts"] = pd.to_datetime(df["ts"], utc=True)

df = df[["symbol", "ts", "open", "high", "low", "close", "volume"]].copy()
df = df.sort_values(["symbol", "ts"]).reset_index(drop=True)

print(f"Loaded rows: {len(df):,}")
print(f"Loaded symbols: {df['symbol'].nunique():,}")


# =========================
# CREATE WICK TEST DATASET
# =========================
df_wicks = df.copy()

rng = np.random.default_rng(RANDOM_SEED)

low_wick_indices = []
high_wick_indices = []

for symbol, group in df_wicks.groupby("symbol"):
    indices = group.index.to_numpy()

    for start in range(0, len(indices), ROWS_PER_GROUP):
        block_indices = indices[start:start + ROWS_PER_GROUP]

        if len(block_indices) < 2:
            continue

        selected = rng.choice(block_indices, size=2, replace=False)

        low_idx = selected[0]
        high_idx = selected[1]

        low_wick_indices.append(low_idx)
        high_wick_indices.append(high_idx)

df_wicks.loc[low_wick_indices, "low"] = df_wicks.loc[low_wick_indices, "low"] * 0.5
df_wicks.loc[high_wick_indices, "high"] = df_wicks.loc[high_wick_indices, "high"] * 2.0


# =========================
# CHECKS
# =========================
print("\n=========================")
print("WICK MODIFICATION CHECKS")
print("=========================")

print(f"Low wick rows modified: {len(low_wick_indices):,}")
print(f"High wick rows modified: {len(high_wick_indices):,}")
print(f"Total modified rows: {len(low_wick_indices) + len(high_wick_indices):,}")

print("\nExample low-wick rows:")
print(df_wicks.loc[low_wick_indices[:10], ["symbol", "ts", "open", "high", "low", "close", "volume"]])

print("\nExample high-wick rows:")
print(df_wicks.loc[high_wick_indices[:10], ["symbol", "ts", "open", "high", "low", "close", "volume"]])


# =========================
# SAVE
# =========================
df_wicks.to_parquet(OUTPUT_FILE, index=False)

print("\nSaved wick test parquet to:")
print(OUTPUT_FILE)