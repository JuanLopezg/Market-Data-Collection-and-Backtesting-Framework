from pathlib import Path
import random
import pandas as pd

# =========================
# CONFIG
# =========================
DIRECTORY = "/mnt/c/Users/Juan/Documents/Python/binance/intraday"

COLUMNS = ["date", "symbol", "open", "high", "low", "close", "volume"]
NUMERIC_COLS = ["open", "high", "low", "close", "volume"]

HOURLY = True

# =========================
# HELPERS
# =========================
def format_number(x):
    if pd.isna(x):
        return ""

    # Fully expanded decimal, no scientific notation
    s = f"{float(x):.12f}"

    # Remove trailing zeros
    s = s.rstrip("0").rstrip(".")

    if s == "-0":
        s = "0"

    return s


def parse_yyyymmddhh(series: pd.Series) -> pd.Series:
    """
    Parses timestamps like 2020010100, 2020010112, etc.
    Returns pandas datetime values.
    """
    s = series.astype(str).str.strip()

    # Handle values that may arrive as floats, e.g. 2020010100.0
    s = s.str.replace(r"\.0$", "", regex=True)

    # Keep digits only, so strings like "2020-01-01 12:00:00" can also work
    digits = s.str.replace(r"\D", "", regex=True)

    # We need YYYYMMDDHH
    digits = digits.str[:10]

    parsed = pd.to_datetime(
        digits,
        format="%Y%m%d%H",
        errors="coerce",
    )

    return parsed


def fake_hourly_dates_as_daily(df: pd.DataFrame) -> pd.Series:
    """
    Converts fake hourly YYYYMMDDHH timestamps into valid daily YYYYMMDD dates.

    Logic:
    - Pick a random symbol with at least 2 timestamps.
    - Calculate the hour difference between its first two consecutive rows.
    - Find the lowest timestamp in the whole dataset.
    - Use the YYYYMMDD part of that lowest timestamp as the base valid date.
    - Convert every timestamp into a fake daily date according to the detected step.

    Examples:
    - If step is 12h:
        2020010100 -> 20200101
        2020010112 -> 20200102

    - If step is 1h:
        2020010100 -> 20200101
        2020010112 -> 20200113
    """
    parsed_dates = parse_yyyymmddhh(df["date"])

    if parsed_dates.isna().any():
        bad_count = parsed_dates.isna().sum()
        raise ValueError(f"Could not parse {bad_count:,} date values as YYYYMMDDHH.")

    temp = df[["symbol"]].copy()
    temp["_parsed_date"] = parsed_dates

    temp = temp.dropna(subset=["symbol", "_parsed_date"])
    temp = temp.sort_values(["symbol", "_parsed_date"])

    eligible_symbols = (
        temp.groupby("symbol")["_parsed_date"]
        .nunique()
    )

    eligible_symbols = eligible_symbols[eligible_symbols >= 2].index.tolist()

    if not eligible_symbols:
        raise ValueError("No symbol has at least two unique timestamps to calculate the hourly step.")

    random_symbol = random.choice(eligible_symbols)

    symbol_dates = (
        temp.loc[temp["symbol"] == random_symbol, "_parsed_date"]
        .drop_duplicates()
        .sort_values()
        .reset_index(drop=True)
    )

    first_ts = symbol_dates.iloc[0]
    second_ts = symbol_dates.iloc[1]

    step_hours = (second_ts - first_ts) / pd.Timedelta(hours=1)

    if step_hours <= 0:
        raise ValueError(f"Invalid hourly step detected for symbol {random_symbol}: {step_hours}")

    min_ts = parsed_dates.min()

    # Base date is the YYYYMMDD part of the lowest timestamp
    base_date = pd.Timestamp(
        year=min_ts.year,
        month=min_ts.month,
        day=min_ts.day,
    )

    hours_from_min = (parsed_dates - min_ts) / pd.Timedelta(hours=1)
    fake_day_offsets = hours_from_min / step_hours

    rounded_offsets = fake_day_offsets.round()

    # Warn if timestamps are not perfectly aligned to the detected step
    max_error = (fake_day_offsets - rounded_offsets).abs().max()
    if max_error > 1e-9:
        print(
            f"Warning: some timestamps are not perfectly aligned with the detected "
            f"{step_hours:g}h step. Rounding fake day offsets."
        )

    fake_dates = base_date + pd.to_timedelta(rounded_offsets.astype(int), unit="D")

    print(f"HOURLY=True")
    print(f"Random symbol used for step detection: {random_symbol}")
    print(f"Detected step: {step_hours:g} hours")
    print(f"Lowest timestamp: {min_ts}")
    print(f"Base fake date: {base_date.strftime('%Y%m%d')}")

    return fake_dates.dt.strftime("%Y%m%d")


def parquet_to_csv(parquet_path: Path):
    print(f"\nLoading: {parquet_path.name}")

    df = pd.read_parquet(parquet_path)

    if "ts" in df.columns:
        df = df.rename(columns={"ts": "date"})

    missing_cols = [
        col for col in ["date", "symbol", "open", "high", "low", "close", "volume"]
        if col not in df.columns
    ]

    if missing_cols:
        print(f"Skipping {parquet_path.name}: missing columns {missing_cols}")
        return

    df = df[["date", "symbol", "open", "high", "low", "close", "volume"]].copy()

    if HOURLY:
        df["date"] = fake_hourly_dates_as_daily(df)
    else:
        df["date"] = pd.to_datetime(df["date"], utc=True).dt.strftime("%Y-%m-%d")

    for col in NUMERIC_COLS:
        df[col] = pd.to_numeric(df[col], errors="coerce").map(format_number)

    csv_path = parquet_path.with_suffix(".csv")

    df.to_csv(
        csv_path,
        index=False,
        columns=COLUMNS,
        lineterminator="\n",
    )

    print(f"Saved: {csv_path.name}")
    print(f"Rows: {len(df):,}")


# =========================
# RUN
# =========================
directory = Path(DIRECTORY)
print("Directory:", directory)
print("Exists:", directory.exists())
print("Is directory:", directory.is_dir())

print("\nContents:")
for item in directory.iterdir():
    print(item.name)

parquet_files = sorted(directory.glob("*.parquet"))

print(f"Found parquet files: {len(parquet_files):,}")

for parquet_file in parquet_files:
    parquet_to_csv(parquet_file)

print("\nDone.")