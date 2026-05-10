import pandas as pd

# =========================
# CONFIG
# =========================
DATA_FILE = "/mnt/c/Users/Juan/Documents/Python/algoTrading/1h_binance.parquet"

SHIFT_HOURS = 4  # ← shift day start (e.g. 4 = day starts at 04:00)

if not (0 <= SHIFT_HOURS < 24):
    raise ValueError("SHIFT_HOURS must be between 0 and 23")


# =========================
# LOAD DATA
# =========================
print("Loading cleaned dataset...")

df = pd.read_parquet(
    DATA_FILE,
    engine="pyarrow"
)

print("Loaded successfully!")
print("\nAvailable symbols:")
print(sorted(df["symbol"].unique()))
df = df.sort_values(["symbol", "ts"]).copy()


# =========================
# FILTER SYMBOLS WITH >= 100 ROWS
# =========================
symbol_counts = df["symbol"].value_counts()
valid_symbols = symbol_counts[symbol_counts >= 1200].index

df = df[df["symbol"].isin(valid_symbols)].copy()

print(f"Symbols kept (>=1200 rows): {len(valid_symbols)}")
print(f"Rows after filtering: {len(df):,}")


# =========================
# SHIFT TIME BASE
# =========================
df["ts_shifted"] = df["ts"] + pd.Timedelta(hours=SHIFT_HOURS)

print(f"\nApplied timestamp shift: +{SHIFT_HOURS} hours")


# =========================
# CREATE DAILY DATAFRAME (24H)
# =========================
df["ts_group"] = df["ts_shifted"].dt.floor("1D")

df_daily = (
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

# Keep only full days (24 hours)
df_daily = df_daily[df_daily["count"] == 24].copy()

df_daily = df_daily.drop(columns=["count"])
df_daily = df_daily.rename(columns={"ts_group": "ts"})
df_daily = df_daily.sort_values(["symbol", "ts"]).reset_index(drop=True)

print(df_daily.head())
print(df_daily.dtypes)

print(f"\nOriginal rows after filtering: {len(df):,}")
print(f"Daily rows: {len(df_daily):,}")


import plotly.graph_objects as go


print("\nAvailable symbols:")
print(sorted(df["symbol"].unique()))
# =========================
# INSPECT BTC
# =========================
symbol = "BTCUSDT"

df_btc_1h = df[df["symbol"] == symbol].copy().sort_values("ts")
df_btc_daily = df_daily[df_daily["symbol"] == symbol].copy().sort_values("ts")

if df_btc_1h.empty or df_btc_daily.empty:
    raise ValueError("BTC data not found")

print("\n=========================")
print("BTC FIRST 30 HOURS")
print("=========================")
print(df_btc_1h.head(30)[["ts", "open", "high", "low", "close", "volume"]])

print("\n=========================")
print("BTC FIRST DAILY CANDLE")
print("=========================")
print(df_btc_daily.head(1)[["ts", "open", "high", "low", "close", "volume"]])

# Optional: show which 24 hourly rows created the first daily candle
first_daily_ts = df_btc_daily.iloc[0]["ts"]

hours_used = df_btc_1h[
    (df_btc_1h["ts_shifted"].dt.floor("1D") == first_daily_ts)
].copy()

print("\n=========================")
print("BTC HOURS USED FOR FIRST DAILY CANDLE")
print("=========================")
print(hours_used[["ts", "ts_shifted", "open", "high", "low", "close", "volume"]])


# =========================
# BTC 1H CHART
# =========================
fig_1h = go.Figure()

fig_1h.add_trace(go.Candlestick(
    x=df_btc_1h["ts"],
    open=df_btc_1h["open"],
    high=df_btc_1h["high"],
    low=df_btc_1h["low"],
    close=df_btc_1h["close"],
    name="BTC 1H"
))

fig_1h.update_layout(
    title="BTC — 1H Candles",
    xaxis_title="Date",
    yaxis_title="Price",
    xaxis_rangeslider_visible=False,
    template="plotly_white",
    height=700
)

file_1h = "/mnt/c/Users/Juan/Documents/Python/BTC_1H_daily_check.html"
fig_1h.write_html(file_1h, auto_open=False)

print(f"\nSaved BTC 1H chart: {file_1h}")


# =========================
# BTC DAILY CHART
# =========================
fig_daily = go.Figure()

fig_daily.add_trace(go.Candlestick(
    x=df_btc_daily["ts"],
    open=df_btc_daily["open"],
    high=df_btc_daily["high"],
    low=df_btc_daily["low"],
    close=df_btc_daily["close"],
    name=f"BTC Daily Shifted {SHIFT_HOURS}H"
))

fig_daily.update_layout(
    title=f"BTC — Daily Candles Shifted {SHIFT_HOURS}H",
    xaxis_title="Date",
    yaxis_title="Price",
    xaxis_rangeslider_visible=False,
    template="plotly_white",
    height=700
)


file_daily = f"/mnt/c/Users/Juan/Documents/Python/BTC_DAILY_shifted_{SHIFT_HOURS}h.html"
fig_daily.write_html(file_daily, auto_open=False)

print(f"Saved BTC daily chart: {file_daily}")


# =========================
# SAVE DATA
# =========================
OUTPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/1d_{SHIFT_HOURS}h_shift_binance.parquet"

df_daily.to_parquet(
    OUTPUT_FILE,
    engine="pyarrow",
    compression="snappy",
    index=False
)

print(f"\nSaved daily dataset to: {OUTPUT_FILE}")






