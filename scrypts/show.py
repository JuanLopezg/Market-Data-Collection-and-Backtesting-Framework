import pandas as pd
import plotly.graph_objects as go

# =========================
# CONFIG
# =========================
PARQUET_FILE = r"C:\Users\Juan\Documents\Python\algoTrading\1d_ohlcv_wicks.parquet"

BTC_SYMBOL = "BTC"

FILE_BTC_HOURLY = r"C:\Users\Juan\Documents\Python\algoTrading\btcusdt_hourly_chart.html"


# =========================
# LOAD DATABASE
# =========================
print("Loading database...")

df = pd.read_parquet(PARQUET_FILE)

df["ts"] = pd.to_datetime(df["ts"], utc=True)

df = df[["symbol", "ts", "open", "high", "low", "close", "volume"]].copy()
df = df.sort_values(["symbol", "ts"]).reset_index(drop=True)

print("Loaded successfully!")
print(f"Total rows: {len(df):,}")
print(f"Total symbols: {df['symbol'].nunique():,}")


# =========================
# BTC CHECK
# =========================
df_btc = df[df["symbol"] == BTC_SYMBOL].sort_values("ts").copy()

if df_btc.empty:
    raise ValueError(f"No rows found for {BTC_SYMBOL}")

print("\n=========================")
print("BTCUSDT HOURLY — FIRST 7 ROWS")
print("=========================")
print(df_btc[["ts", "symbol", "open", "high", "low", "close", "volume"]].head(7))

print("\n=========================")
print("BTCUSDT HOURLY — LAST 7 ROWS")
print("=========================")
print(df_btc[["ts", "symbol", "open", "high", "low", "close", "volume"]].tail(7))

print(f"\nBTCUSDT rows: {len(df_btc):,}")


# =========================
# BTC HOURLY CHART
# =========================
fig = go.Figure()

fig.add_trace(
    go.Candlestick(
        x=df_btc["ts"],
        open=df_btc["open"],
        high=df_btc["high"],
        low=df_btc["low"],
        close=df_btc["close"],
        name="BTCUSDT Hourly",
    )
)

fig.update_layout(
    title="BTCUSDT — Binance Futures Hourly Candles",
    xaxis_title="Date",
    yaxis_title="Price",
    xaxis_rangeslider_visible=False,
    template="plotly_white",
    height=700,
)

fig.write_html(FILE_BTC_HOURLY, auto_open=True)

print(f"\nSaved BTCUSDT hourly chart: {FILE_BTC_HOURLY}")