import pandas as pd
import plotly.graph_objects as go

# =========================
# CONFIG
# =========================
PARQUET_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/1d_binance.parquet"


GROUP_DAYS = 30  # must be int > 1
REFERENCE_DATE = pd.Timestamp("1999-01-01", tz="UTC")

BTC_SYMBOL = "BTC"  # change if your symbol is different

FILE_DAILY = f"/mnt/c/Users/Juan/Documents/Python/BTC_DAILY_original.html"
FILE_XD_DAILY = f"/mnt/c/Users/Juan/Documents/Python/BTC_{GROUP_DAYS}D_grouped.html"

if not isinstance(GROUP_DAYS, int) or GROUP_DAYS <= 1:
    raise ValueError("GROUP_DAYS must be an integer > 1")

# =========================
# LOAD PARQUET
# =========================
print("Loading Parquet...")
df = pd.read_parquet(PARQUET_FILE)
print("Loaded successfully!\n")

# =========================
# PREPARE DATA
# =========================
df["ts"] = pd.to_datetime(df["ts"], utc=True)
df = df.sort_values(["symbol", "ts"]).reset_index(drop=True)

print("DataFrame info:")
df.info()

print(f"\nTotal rows: {len(df):,}")
print(f"Symbols: {df['symbol'].nunique():,}")

# =========================
# GROUP X-DAY CANDLES
# =========================
days_from_ref = (df["ts"].dt.normalize() - REFERENCE_DATE).dt.days

df["period_id"] = days_from_ref // GROUP_DAYS

df["period_ts"] = REFERENCE_DATE + pd.to_timedelta(
    df["period_id"] * GROUP_DAYS,
    unit="D"
)

df_grouped = (
    df
    .sort_values(["symbol", "ts"])
    .groupby(["symbol", "period_id"], as_index=False)
    .agg(
        ts=("period_ts", "first"),
        open=("open", "first"),
        high=("high", "max"),
        low=("low", "min"),
        close=("close", "last"),
        volume=("volume", "sum"),
    )
)

df_grouped = df_grouped[
    ["ts", "symbol", "open", "high", "low", "close", "volume"]
]

print(f"\nRows before: {len(df):,}")
print(f"Rows after grouping {GROUP_DAYS}D: {len(df_grouped):,}")

# =========================
# BTC CHECK
# =========================
df_btc_daily = df[df["symbol"] == BTC_SYMBOL].sort_values("ts").copy()
df_btc_xd_daily = df_grouped[df_grouped["symbol"] == BTC_SYMBOL].sort_values("ts").copy()

print("\n=========================")
print("BTC ORIGINAL DAILY — FIRST 7 ROWS")
print("=========================")
print(df_btc_daily[["ts", "symbol", "open", "high", "low", "close", "volume"]].head(7))

print(f"\n=========================")
print(f"BTC GROUPED {GROUP_DAYS}D — FIRST 7 ROWS")
print("=========================")
print(df_btc_xd_daily[["ts", "symbol", "open", "high", "low", "close", "volume"]].head(7))

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
    name="BTC Daily"
))

fig_daily.update_layout(
    title="BTC — Original Daily Candles",
    xaxis_title="Date",
    yaxis_title="Price",
    xaxis_rangeslider_visible=False,
    template="plotly_white",
    height=700
)

fig_daily.write_html(FILE_DAILY, auto_open=False)

print(f"\nSaved BTC original daily chart: {FILE_DAILY}")

# =========================
# BTC X-DAY DAILY CHART
# =========================
fig_xd_daily = go.Figure()

fig_xd_daily.add_trace(go.Candlestick(
    x=df_btc_xd_daily["ts"],
    open=df_btc_xd_daily["open"],
    high=df_btc_xd_daily["high"],
    low=df_btc_xd_daily["low"],
    close=df_btc_xd_daily["close"],
    name=f"BTC {GROUP_DAYS}D"
))

fig_xd_daily.update_layout(
    title=f"BTC — Grouped {GROUP_DAYS}D Candles",
    xaxis_title="Date",
    yaxis_title="Price",
    xaxis_rangeslider_visible=False,
    template="plotly_white",
    height=700
)

fig_xd_daily.write_html(FILE_XD_DAILY, auto_open=False)

print(f"Saved BTC grouped {GROUP_DAYS}D chart: {FILE_XD_DAILY}")


# =========================
# SAVE GROUPED PARQUET
# =========================
OUTPUT_FILE = f"/mnt/c/Users/Juan/Documents/Python/algoTrading/{GROUP_DAYS}d_binance.parquet"

df_grouped.to_parquet(OUTPUT_FILE, index=False)

print(f"\nSaved grouped parquet: {OUTPUT_FILE}")