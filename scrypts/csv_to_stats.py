import pandas as pd
from pathlib import Path

# Change these paths
INPUT_CSV = "/mnt/c/Users/Juan/Documents/Python/algoTrading/ll.CSV"
OUTPUT_PARQUET = "/mnt/c/Users/Juan/Documents/Python/algoTrading/stats.parquet"

COLUMNS = [
    "Test", "Name", "Dates", "Periods", "NetProfit", "Comp", "ROR", "MaxDD",
    "MAR", "Trades", "PctWins", "Expectancy", "AvgWin", "AvgLoss", "WinLen",
    "LossLen", "ProfitFactor", "Sharpe", "AvgExp", "MaxExp"
]

N_STRATEGIES = 40
TIMEFRAMES = ["12h", "1db", "1dc"]


def load_strategy_csv(input_csv: str) -> pd.DataFrame:
    # Skip the first line and manually assign column names
    df = pd.read_csv(
        input_csv,
        skiprows=1,
        names=COLUMNS
    )

    expected_rows = N_STRATEGIES * len(TIMEFRAMES)

    if len(df) != expected_rows:
        raise ValueError(
            f"Expected {expected_rows} data rows, but found {len(df)} rows."
        )

    # 0-based row number inside the full CSV data section
    df["_row_number"] = range(len(df))

    # Block: 0 = first 40 rows, 1 = second 40, 2 = third 40
    df["_block"] = df["_row_number"] // N_STRATEGIES

    # Position inside each block: 0..39
    df["_position_in_block"] = df["_row_number"] % N_STRATEGIES

    # Inverse strategy ID:
    # first row in each block is strategy 40
    # last row in each block is strategy 1
    df["strategy_id"] = N_STRATEGIES - df["_position_in_block"]

    # Add timeframe based on block
    df["timeframe"] = df["_block"].map({
        0: "12h",
        1: "1db",
        2: "1dc",
    })

    # Put identifier columns first
    df = df[["strategy_id", "timeframe"] + COLUMNS]

    # Sort so strategy 1 comes first, with its 3 timeframes together
    timeframe_order = pd.CategoricalDtype(TIMEFRAMES, ordered=True)
    df["timeframe"] = df["timeframe"].astype(timeframe_order)

    df = df.sort_values(["strategy_id", "timeframe"]).reset_index(drop=True)

    return df


def main():
    input_path = Path(INPUT_CSV)
    output_path = Path(OUTPUT_PARQUET)

    df = load_strategy_csv(input_path)

    print("\nStrategy 1 rows:")
    print(df[df["strategy_id"] == 1].to_string(index=False))

    print("\nStrategy 40 rows:")
    print(df[df["strategy_id"] == 40].to_string(index=False))

    df.to_parquet(output_path, index=False)

    print(f"\nSaved parquet to: {output_path}")


if __name__ == "__main__":
    main()