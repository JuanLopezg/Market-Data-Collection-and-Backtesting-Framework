import pandas as pd
from pathlib import Path

PARQUET_PATH = Path("/mnt/c/Users/Juan/Documents/Python/algoTrading/stats.parquet")


def clean_numeric(series: pd.Series) -> pd.Series:
    """
    Converts values like '$1,075,572', '48.14%', '-51.39%' into floats.
    Works even if the column is already numeric.
    """
    return (
        series.astype(str)
        .str.replace("$", "", regex=False)
        .str.replace(",", "", regex=False)
        .str.replace("%", "", regex=False)
        .str.strip()
        .pipe(pd.to_numeric, errors="coerce")
    )


def unique_join(values) -> str:
    seen = []
    for value in values:
        if value not in seen:
            seen.append(value)
    return "; ".join(seen)


def get_manual_1dc_reasons(row) -> list[str]:
    reasons = []

    pf_1dc = row["ProfitFactor"]
    mar_1dc = row["MAR"]
    sharpe_1dc = row["Sharpe"]
    trades_1dc = row["Trades"]
    exp_1dc = row["Expectancy"]
    avgloss_1dc = row["AvgLoss"]
    maxdd_1dc = abs(row["MaxDD"])
    avgexp_1dc = row["AvgExp"]
    maxexp_1dc = row["MaxExp"]
    pctwins_1dc = row["PctWins"]
    avgwin_1dc = row["AvgWin"]
    losslen_1dc = row["LossLen"]
    winlen_1dc = row["WinLen"]

    if 1.05 <= pf_1dc < 1.15:
        reasons.append(f"1dc ProfitFactor between 1.05 and 1.15 ({pf_1dc})")

    if 0.25 <= mar_1dc < 0.50:
        reasons.append(f"1dc MAR between 0.25 and 0.50 ({mar_1dc})")

    if 0.4 <= sharpe_1dc < 0.8:
        reasons.append(f"1dc Sharpe between 0.4 and 0.8 ({sharpe_1dc})")

    if 40 <= trades_1dc < 100:
        reasons.append(f"1dc Trades between 40 and 100 ({trades_1dc})")

    if exp_1dc > 0 and avgloss_1dc > 0 and exp_1dc / avgloss_1dc < 0.05:
        reasons.append(
            f"1dc Expectancy / AvgLoss < 0.05 "
            f"({exp_1dc} / {avgloss_1dc} = {exp_1dc / avgloss_1dc:.4f})"
        )

    if pf_1dc < 1.15 and mar_1dc < 0.50 and sharpe_1dc < 0.80:
        reasons.append(
            f"1dc weak combo: PF < 1.15, MAR < 0.50, Sharpe < 0.80 "
            f"({pf_1dc}, {mar_1dc}, {sharpe_1dc})"
        )

    if maxdd_1dc > 30 and mar_1dc < 0.75:
        reasons.append(
            f"1dc MaxDD > 30 and MAR < 0.75 "
            f"({maxdd_1dc}, {mar_1dc})"
        )

    if avgexp_1dc > 70 and pf_1dc < 1.20:
        reasons.append(
            f"1dc AvgExp > 70 and PF < 1.20 "
            f"({avgexp_1dc}, {pf_1dc})"
        )

    if maxexp_1dc > 90 and mar_1dc < 0.75:
        reasons.append(
            f"1dc MaxExp > 90 and MAR < 0.75 "
            f"({maxexp_1dc}, {mar_1dc})"
        )

    if pctwins_1dc > 55 and avgwin_1dc < 0.80 * avgloss_1dc:
        reasons.append(
            f"1dc PctWins > 55 and AvgWin < 0.80 * AvgLoss "
            f"({pctwins_1dc}, {avgwin_1dc}, {avgloss_1dc})"
        )

    if pctwins_1dc < 40 and avgloss_1dc > 0 and avgwin_1dc / avgloss_1dc < 2.0:
        reasons.append(
            f"1dc PctWins < 40 and AvgWin / AvgLoss < 2.0 "
            f"({pctwins_1dc}, {avgwin_1dc} / {avgloss_1dc} = {avgwin_1dc / avgloss_1dc:.4f})"
        )

    if losslen_1dc > 2 * winlen_1dc and pf_1dc < 1.25:
        reasons.append(
            f"1dc LossLen > 2 * WinLen and PF < 1.25 "
            f"({losslen_1dc}, {winlen_1dc}, {pf_1dc})"
        )

    return reasons


def get_cross_dataset_reasons(group: pd.DataFrame) -> list[str]:
    reasons = []

    negative_ror_tfs = group.loc[group["ROR"] < 0, "timeframe"].tolist()
    negative_netprofit_tfs = group.loc[group["NetProfit"] < 0, "timeframe"].tolist()

    ror_values = group["ROR"].dropna()

    if len(negative_ror_tfs) == 1:
        reasons.append(f"Exactly one timeframe has negative ROR: {negative_ror_tfs}")

    if len(negative_netprofit_tfs) == 1:
        reasons.append(f"Exactly one timeframe has negative NetProfit: {negative_netprofit_tfs}")

    if not ror_values.empty:
        min_ror = ror_values.min()
        max_ror = ror_values.max()

        if min_ror > 0 and max_ror > 0 and min_ror / max_ror < 0.30:
            reasons.append(
                f"ROR imbalance: min_ror / max_ror < 0.30 "
                f"({min_ror} / {max_ror} = {min_ror / max_ror:.4f})"
            )

        if min_ror < 0 < max_ror:
            reasons.append(
                f"Mixed positive/negative ROR across timeframes "
                f"(min={min_ror}, max={max_ror})"
            )

    pf_by_tf = group.set_index("timeframe")["ProfitFactor"].to_dict()

    pf_1dc = pf_by_tf.get("1dc")
    pf_1db = pf_by_tf.get("1db")
    pf_12h = pf_by_tf.get("12h")

    if pd.notna(pf_1dc) and pf_1dc > 1:
        other_pfs = [
            pf for pf in [pf_1db, pf_12h]
            if pd.notna(pf)
        ]

        if other_pfs and max(other_pfs) >= pf_1dc * 1.40:
            reasons.append(
                f"PF instability: max(1db, 12h) >= 1dc PF * 1.40 "
                f"({max(other_pfs)} >= {pf_1dc * 1.40:.4f})"
            )

    return reasons


def main():
    df = pd.read_parquet(PARQUET_PATH)

    # First modify all strategy IDs:
    # 1 -> 0, 40 -> 39, etc.
    df["strategy_id"] = df["strategy_id"] - 1

    # Numeric working copy
    work_df = df.copy()

    numeric_cols = [
        "NetProfit",
        "ROR",
        "Expectancy",
        "ProfitFactor",
        "Trades",
        "MAR",
        "Sharpe",
        "AvgLoss",
        "MaxDD",
        "AvgExp",
        "MaxExp",
        "PctWins",
        "AvgWin",
        "LossLen",
        "WinLen",
    ]

    for col in numeric_cols:
        work_df[col] = clean_numeric(work_df[col])

    # ------------------------------------------------------------
    # HARD DELETE RULES
    # These are applied first.
    # Details are not printed.
    # ------------------------------------------------------------

    quality_timeframes = work_df[work_df["timeframe"].isin(["1dc", "1db"])].copy()

    bad_quality_rows = quality_timeframes[
        (quality_timeframes["NetProfit"] <= 0)
        | (quality_timeframes["ROR"] <= 0)
        | (quality_timeframes["Expectancy"] <= 0)
        | (quality_timeframes["ProfitFactor"] < 1.05)
        | (quality_timeframes["Trades"] < 40)
        | (quality_timeframes["MAR"] < 0.25)
        | (
            (quality_timeframes["ProfitFactor"] < 1.10)
            & (quality_timeframes["Sharpe"] < 0.4)
        )
    ].copy()

    twelve_h = work_df[work_df["timeframe"] == "12h"].copy()

    bad_12h_rows = twelve_h[
        twelve_h["NetProfit"] < 0
    ].copy()

    hard_delete_ids = pd.concat(
        [
            bad_quality_rows["strategy_id"],
            bad_12h_rows["strategy_id"],
        ],
        ignore_index=True,
    ).drop_duplicates()

    hard_filtered_df = df[~df["strategy_id"].isin(hard_delete_ids)].copy()
    hard_filtered_work_df = work_df[~work_df["strategy_id"].isin(hard_delete_ids)].copy()

    # ------------------------------------------------------------
    # MANUAL-REVIEW DELETE RULES
    # These are applied only after hard deletes.
    # These are the only deleted strategies printed.
    # ------------------------------------------------------------

    manual_records = []

    # 1dc-specific manual-review conditions
    remaining_1dc = hard_filtered_work_df[
        hard_filtered_work_df["timeframe"] == "1dc"
    ].copy()

    for _, row in remaining_1dc.iterrows():
        reasons = get_manual_1dc_reasons(row)

        if reasons:
            manual_records.append(
                {
                    "strategy_id": row["strategy_id"],
                    "ManualReviewReasons": "; ".join(reasons),
                }
            )

    # Cross-dataset manual-review conditions
    for strategy_id, group in hard_filtered_work_df.groupby("strategy_id"):
        reasons = get_cross_dataset_reasons(group)

        if reasons:
            manual_records.append(
                {
                    "strategy_id": strategy_id,
                    "ManualReviewReasons": "; ".join(reasons),
                }
            )

    if manual_records:
        manual_review_deleted_df = pd.DataFrame(manual_records)

        manual_review_deleted_df = (
            manual_review_deleted_df
            .groupby("strategy_id", as_index=False)["ManualReviewReasons"]
            .agg(unique_join)
            .sort_values("strategy_id")
        )
    else:
        manual_review_deleted_df = pd.DataFrame(
            columns=["strategy_id", "ManualReviewReasons"]
        )

    manual_review_delete_ids = manual_review_deleted_df["strategy_id"].unique()

    final_filtered_df = hard_filtered_df[
        ~hard_filtered_df["strategy_id"].isin(manual_review_delete_ids)
    ].copy()

    print(f"Original rows after ID shift: {len(df)}")
    print(f"Rows after hard deletes: {len(hard_filtered_df)}")
    print(f"Hard-deleted strategies: {len(hard_delete_ids)}")
    for strat in hard_delete_ids :
        print(f"Hard-deleted strat: {strat}")

    print(f"Manual-review deleted strategies: {len(manual_review_delete_ids)}")
    print(f"Final rows after manual-review deletes: {len(final_filtered_df)}")

    print("\nManual-review deleted strategies:")
    if manual_review_deleted_df.empty:
        print("None")
    else:
        print(manual_review_deleted_df.to_string(index=False))

    # Use this for the next subtraction step:
    # final_filtered_df = remaining dataframe after hard + manual-review deletes
    # manual_review_deleted_df = only the new manual-review deletions with reasons


if __name__ == "__main__":
    main()