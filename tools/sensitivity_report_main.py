#!/usr/bin/env python3
"""Build an in-sample parameter-sensitivity report from CSV study outputs.

This tool is deliberately separate from the C++ backtest executable. It reads
one completed study directory, checks the raw grid results, then writes a static
HTML report plus PNG charts and CSV tables for later inspection.

It does NOT run backtests, choose production parameters, model costs/slippage,
or perform out-of-sample validation.
"""

from __future__ import annotations

import argparse
import copy
import html
import json
import math
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


META_COLUMNS = {
    "study_id",
    "strategy",
    "run_id",
    "status",
    "failure_reason",
}

METRIC_COLUMNS = {
    "net_return_percent",
    "annualized_return_percent",
    "max_drawdown_percent",
    "sharpe_ratio",
    "sortino_ratio",
    "calmar_ratio",
    "profit_factor",
    "expectancy_per_trade",
    "trade_count",
    "exposure_percent",
    "turnover_multiple",
    "win_rate_percent",
    "average_win",
    "average_loss",
    "max_consecutive_losses",
    "worst_loss_streak",
    "average_holding_bars",
    "net_profit",
    "final_equity",
}

NUMERIC_COLUMNS = METRIC_COLUMNS | {"run_id"}

DEFAULT_CONFIG: dict[str, Any] = {
    "filters": {
        "min_trade_count": 100,
        "min_net_return_percent": 0.0,
        "max_drawdown_percent": 35.0,
        "min_calmar_ratio": 1.0,
        "min_profit_factor": 1.10,
    },
    "robustness": {
        "min_existing_neighbours": 2,
        "min_neighbour_pass_rate": 0.50,
        "top_candidates_to_show": 20,
    },
    "verdict": {
        "minimum_successful_runs": 20,
        "minimum_filter_pass_rate": 0.05,
        "minimum_robust_candidates": 3,
    },
    "strategy_overrides": {},
}


@dataclass
class StrategyResult:
    strategy: str
    active_parameters: list[str]
    output_dir: Path
    summary_row: dict[str, Any]
    report_html: str


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    """Return a recursive merge without mutating either argument."""
    merged = copy.deepcopy(base)
    for key, value in override.items():
        if (
            key in merged
            and isinstance(merged[key], dict)
            and isinstance(value, dict)
        ):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create an HTML sensitivity-analysis report from a study directory."
    )
    parser.add_argument(
        "--study-dir",
        required=True,
        type=Path,
        help="Directory containing strategy CSVs, parameter_grid.csv, and study_metadata.csv.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory. Defaults to <study-dir>/analysis_report.",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        help="Optional JSON configuration file with filter/robustness thresholds.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow writing into an existing output directory.",
    )
    return parser.parse_args()


def load_config(config_path: Path | None) -> dict[str, Any]:
    if config_path is None:
        return copy.deepcopy(DEFAULT_CONFIG)

    if not config_path.is_file():
        raise FileNotFoundError(f"Configuration file not found: {config_path}")

    with config_path.open("r", encoding="utf-8") as handle:
        supplied = json.load(handle)

    if not isinstance(supplied, dict):
        raise ValueError("Configuration root must be a JSON object.")

    return deep_merge(DEFAULT_CONFIG, supplied)


def format_number(value: Any, decimals: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "—"
    try:
        number = float(value)
    except (TypeError, ValueError):
        return html.escape(str(value))
    if not math.isfinite(number):
        return "—"
    return f"{number:,.{decimals}f}"


def format_percent(value: Any, decimals: int = 2) -> str:
    return f"{format_number(value, decimals)}%" if format_number(value, decimals) != "—" else "—"


def safe_filename(value: str) -> str:
    return "".join(char if (char.isalnum() or char in "-_.") else "_" for char in value)


def read_csv_or_raise(path: Path) -> pd.DataFrame:
    try:
        return pd.read_csv(path)
    except Exception as exc:  # pandas errors vary by version.
        raise RuntimeError(f"Could not read CSV '{path}': {exc}") from exc


def strategy_csv_paths(study_dir: Path) -> list[Path]:
    excluded = {"parameter_grid.csv", "study_metadata.csv"}
    paths = []
    for path in sorted(study_dir.glob("*.csv")):
        if path.name in excluded:
            continue
        try:
            preview = pd.read_csv(path, nrows=2)
        except Exception:
            continue
        if {"strategy", "status", "run_id"}.issubset(preview.columns):
            paths.append(path)
    return paths


def enabled_parameters_from_grid(
    parameter_grid: pd.DataFrame | None,
    strategy: str,
    frame: pd.DataFrame,
) -> list[str]:
    """Use parameter_grid.csv when available; otherwise infer varying inputs."""
    if parameter_grid is not None:
        required = {"strategy", "parameter_name", "enabled"}
        if required.issubset(parameter_grid.columns):
            relevant = parameter_grid[
                parameter_grid["strategy"].astype(str).eq(strategy)
            ].copy()
            if not relevant.empty:
                enabled = relevant[
                    relevant["enabled"].astype(str).str.lower().eq("yes")
                ]["parameter_name"].astype(str).tolist()
                present_and_varying = [
                    parameter
                    for parameter in enabled
                    if parameter in frame.columns
                    and frame[parameter].dropna().nunique() > 1
                ]
                if present_and_varying:
                    return present_and_varying

    candidates = [
        column
        for column in frame.columns
        if column not in META_COLUMNS
        and column not in METRIC_COLUMNS
        and frame[column].dropna().nunique() > 1
    ]
    return candidates


def normalise_frame(frame: pd.DataFrame) -> pd.DataFrame:
    result = frame.copy()
    for column in result.columns:
        if column in NUMERIC_COLUMNS or column not in META_COLUMNS:
            converted = pd.to_numeric(result[column], errors="coerce")
            # Only replace columns that actually look numeric. This preserves
            # failure_reason and any future textual metadata.
            if converted.notna().sum() > 0 or column in NUMERIC_COLUMNS:
                result[column] = converted

    if "status" not in result.columns:
        raise ValueError("Missing required 'status' column.")
    result["status"] = result["status"].fillna("missing").astype(str).str.lower()
    if "failure_reason" not in result.columns:
        result["failure_reason"] = ""
    result["failure_reason"] = result["failure_reason"].fillna("").astype(str)
    return result


def strategy_config(config: dict[str, Any], strategy: str) -> dict[str, Any]:
    override = config.get("strategy_overrides", {}).get(strategy, {})
    return deep_merge(
        {
            "filters": config["filters"],
            "robustness": config["robustness"],
            "verdict": config["verdict"],
        },
        override,
    )


def finite_metric_mask(frame: pd.DataFrame) -> pd.Series:
    required = [
        "net_return_percent",
        "max_drawdown_percent",
        "calmar_ratio",
        "profit_factor",
        "trade_count",
    ]
    missing = [column for column in required if column not in frame.columns]
    if missing:
        raise ValueError(
            "The strategy CSV is missing required analysis columns: " + ", ".join(missing)
        )
    return np.isfinite(frame[required]).all(axis=1)


def pareto_frontier_mask(frame: pd.DataFrame) -> pd.Series:
    """True for points not dominated on return (higher) and drawdown (lower)."""
    if frame.empty:
        return pd.Series(False, index=frame.index)

    ordered = frame.sort_values(
        ["net_return_percent", "max_drawdown_percent"],
        ascending=[False, True],
        kind="mergesort",
    )
    best_drawdown = math.inf
    selected: list[Any] = []
    for index, row in ordered.iterrows():
        drawdown = float(row["max_drawdown_percent"])
        if drawdown < best_drawdown - 1e-12:
            selected.append(index)
            best_drawdown = drawdown
    return frame.index.isin(selected)


def make_combo_key(row: pd.Series, parameters: list[str]) -> tuple[float, ...]:
    return tuple(round(float(row[parameter]), 12) for parameter in parameters)


def add_neighbourhood_metrics(
    successful: pd.DataFrame,
    active_parameters: list[str],
    config: dict[str, Any],
) -> pd.DataFrame:
    result = successful.copy()
    result["existing_neighbour_count"] = 0
    result["passing_neighbour_count"] = 0
    result["neighbour_pass_rate"] = np.nan
    result["is_robust_candidate"] = False

    if not active_parameters or result.empty:
        return result

    working = result.dropna(subset=active_parameters).copy()
    if working.empty:
        return result

    value_lists: dict[str, list[float]] = {
        parameter: sorted(working[parameter].dropna().unique().tolist())
        for parameter in active_parameters
    }
    value_positions: dict[str, dict[float, int]] = {
        parameter: {round(float(value), 12): index for index, value in enumerate(values)}
        for parameter, values in value_lists.items()
    }

    combo_to_pass: dict[tuple[float, ...], bool] = {}
    for _, row in working.iterrows():
        combo_to_pass[make_combo_key(row, active_parameters)] = bool(row["passes_filters"])

    counts: dict[Any, tuple[int, int]] = {}
    for index, row in working.iterrows():
        current = list(make_combo_key(row, active_parameters))
        existing = 0
        passing = 0
        for parameter_index, parameter in enumerate(active_parameters):
            current_value = current[parameter_index]
            position = value_positions[parameter].get(current_value)
            if position is None:
                continue
            for delta in (-1, 1):
                neighbour_position = position + delta
                if neighbour_position < 0 or neighbour_position >= len(value_lists[parameter]):
                    continue
                neighbour = current.copy()
                neighbour[parameter_index] = round(
                    float(value_lists[parameter][neighbour_position]), 12
                )
                neighbour_key = tuple(neighbour)
                if neighbour_key in combo_to_pass:
                    existing += 1
                    passing += int(combo_to_pass[neighbour_key])
        counts[index] = (existing, passing)

    for index, (existing, passing) in counts.items():
        result.loc[index, "existing_neighbour_count"] = existing
        result.loc[index, "passing_neighbour_count"] = passing
        result.loc[index, "neighbour_pass_rate"] = (
            passing / existing if existing > 0 else np.nan
        )

    robustness = config["robustness"]
    result["is_robust_candidate"] = (
        result["passes_filters"]
        & (result["existing_neighbour_count"] >= robustness["min_existing_neighbours"])
        & (result["neighbour_pass_rate"] >= robustness["min_neighbour_pass_rate"])
    )
    return result


def apply_filters(successful: pd.DataFrame, filters: dict[str, Any]) -> pd.DataFrame:
    result = successful.copy()
    result["passes_filters"] = (
        (result["trade_count"] >= float(filters["min_trade_count"]))
        & (result["net_return_percent"] > float(filters["min_net_return_percent"]))
        & (result["max_drawdown_percent"] <= float(filters["max_drawdown_percent"]))
        & (result["calmar_ratio"] >= float(filters["min_calmar_ratio"]))
        & (result["profit_factor"] >= float(filters["min_profit_factor"]))
    )
    return result


def describe_metric(series: pd.Series) -> dict[str, float]:
    values = pd.to_numeric(series, errors="coerce").replace([np.inf, -np.inf], np.nan).dropna()
    if values.empty:
        return {
            "mean": np.nan,
            "median": np.nan,
            "p25": np.nan,
            "p75": np.nan,
            "min": np.nan,
            "max": np.nan,
        }
    return {
        "mean": float(values.mean()),
        "median": float(values.median()),
        "p25": float(values.quantile(0.25)),
        "p75": float(values.quantile(0.75)),
        "min": float(values.min()),
        "max": float(values.max()),
    }


def save_distribution_chart(successful: pd.DataFrame, output_path: Path, strategy: str) -> None:
    metrics = [
        ("net_return_percent", "Net return (%)"),
        ("max_drawdown_percent", "Maximum drawdown (%)"),
        ("calmar_ratio", "Calmar ratio"),
        ("trade_count", "Trade count"),
    ]
    figure, axes = plt.subplots(2, 2, figsize=(12, 8))
    for axis, (column, title) in zip(axes.flat, metrics):
        values = successful[column].replace([np.inf, -np.inf], np.nan).dropna()
        if values.empty:
            axis.text(0.5, 0.5, "No finite values", ha="center", va="center")
        else:
            bins = min(40, max(10, int(math.sqrt(len(values)) * 2)))
            axis.hist(values, bins=bins, edgecolor="black", linewidth=0.4)
            axis.axvline(values.median(), linestyle="--", label="Median")
            axis.axvline(values.mean(), linestyle=":", label="Mean")
            axis.legend(fontsize=8)
        axis.set_title(title)
        axis.set_ylabel("Combinations")
    figure.suptitle(f"{strategy}: distribution of successful combinations")
    figure.tight_layout()
    figure.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(figure)


def save_return_drawdown_chart(successful: pd.DataFrame, output_path: Path, strategy: str) -> None:
    figure, axis = plt.subplots(figsize=(9, 6))
    rejected = successful[~successful["passes_filters"]]
    accepted = successful[successful["passes_filters"]]
    robust = successful[successful["is_robust_candidate"]]
    pareto = successful[successful["is_pareto"]]

    if not rejected.empty:
        axis.scatter(
            rejected["max_drawdown_percent"],
            rejected["net_return_percent"],
            alpha=0.45,
            label="Does not pass filters",
        )
    if not accepted.empty:
        axis.scatter(
            accepted["max_drawdown_percent"],
            accepted["net_return_percent"],
            alpha=0.75,
            marker="s",
            label="Passes filters",
        )
    if not robust.empty:
        axis.scatter(
            robust["max_drawdown_percent"],
            robust["net_return_percent"],
            s=60,
            marker="*",
            label="Robust candidate",
        )
    if not pareto.empty:
        frontier = pareto.sort_values("max_drawdown_percent")
        axis.plot(
            frontier["max_drawdown_percent"],
            frontier["net_return_percent"],
            linestyle="--",
            linewidth=1.2,
            label="Return/DD Pareto frontier",
        )

    axis.set_xlabel("Maximum drawdown (%) — lower is better")
    axis.set_ylabel("Net return (%) — higher is better")
    axis.set_title(f"{strategy}: return versus drawdown")
    axis.grid(alpha=0.25)
    axis.legend(fontsize=8)
    figure.tight_layout()
    figure.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(figure)


def make_heatmap_axis(
    axis: plt.Axes,
    matrix: pd.DataFrame,
    title: str,
    x_parameter: str,
    y_parameter: str,
    percent: bool = False,
) -> None:
    if matrix.empty:
        axis.text(0.5, 0.5, "No observations", ha="center", va="center")
        axis.set_title(title)
        return

    values = matrix.to_numpy(dtype=float)
    image = axis.imshow(values, aspect="auto")
    axis.set_title(title)
    axis.set_xlabel(x_parameter)
    axis.set_ylabel(y_parameter)
    axis.set_xticks(np.arange(len(matrix.columns)))
    axis.set_xticklabels([format_number(value, 4) for value in matrix.columns], rotation=45, ha="right", fontsize=7)
    axis.set_yticks(np.arange(len(matrix.index)))
    axis.set_yticklabels([format_number(value, 4) for value in matrix.index], fontsize=7)
    colorbar = plt.colorbar(image, ax=axis, fraction=0.046, pad=0.04)
    colorbar.ax.tick_params(labelsize=7)

    if values.size <= 100:
        for row_index in range(values.shape[0]):
            for column_index in range(values.shape[1]):
                value = values[row_index, column_index]
                if np.isfinite(value):
                    label = f"{value:.1f}" if percent else f"{value:.2f}"
                    axis.text(
                        column_index,
                        row_index,
                        label,
                        ha="center",
                        va="center",
                        fontsize=6,
                    )


def save_pair_heatmap(
    successful: pd.DataFrame,
    parameter_x: str,
    parameter_y: str,
    output_path: Path,
    strategy: str,
) -> pd.DataFrame:
    grouped = (
        successful.groupby([parameter_y, parameter_x], dropna=False)
        .agg(
            median_net_return_percent=("net_return_percent", "median"),
            median_max_drawdown_percent=("max_drawdown_percent", "median"),
            median_calmar_ratio=("calmar_ratio", "median"),
            pass_rate=("passes_filters", "mean"),
            combination_count=("run_id", "count"),
        )
        .reset_index()
    )

    figure, axes = plt.subplots(2, 2, figsize=(14, 10))
    specifications = [
        ("median_net_return_percent", "Median net return (%)", True),
        ("median_max_drawdown_percent", "Median max drawdown (%)", True),
        ("median_calmar_ratio", "Median Calmar ratio", False),
        ("pass_rate", "Filter pass rate", True),
    ]
    for axis, (column, title, percent) in zip(axes.flat, specifications):
        matrix = grouped.pivot(index=parameter_y, columns=parameter_x, values=column)
        make_heatmap_axis(axis, matrix, title, parameter_x, parameter_y, percent=percent)
    figure.suptitle(
        f"{strategy}: {parameter_x} × {parameter_y}\n"
        "Each cell aggregates remaining active parameters with the median; pass rate is the share meeting all filters."
    )
    figure.tight_layout()
    figure.savefig(output_path, dpi=170, bbox_inches="tight")
    plt.close(figure)
    return grouped


def save_marginal_chart(
    successful: pd.DataFrame,
    parameter: str,
    output_path: Path,
    strategy: str,
) -> pd.DataFrame:
    grouped = (
        successful.groupby(parameter, dropna=False)
        .agg(
            combination_count=("run_id", "count"),
            mean_net_return_percent=("net_return_percent", "mean"),
            median_net_return_percent=("net_return_percent", "median"),
            p25_net_return_percent=("net_return_percent", lambda values: values.quantile(0.25)),
            p75_net_return_percent=("net_return_percent", lambda values: values.quantile(0.75)),
            mean_max_drawdown_percent=("max_drawdown_percent", "mean"),
            median_max_drawdown_percent=("max_drawdown_percent", "median"),
            p75_max_drawdown_percent=("max_drawdown_percent", lambda values: values.quantile(0.75)),
            median_calmar_ratio=("calmar_ratio", "median"),
            filter_pass_rate=("passes_filters", "mean"),
        )
        .reset_index()
        .sort_values(parameter)
    )

    figure, axes = plt.subplots(3, 1, figsize=(10, 10), sharex=True)
    x = grouped[parameter]

    axes[0].plot(x, grouped["median_net_return_percent"], marker="o", label="Median return")
    axes[0].plot(x, grouped["mean_net_return_percent"], marker="x", label="Mean return")
    axes[0].fill_between(
        x,
        grouped["p25_net_return_percent"],
        grouped["p75_net_return_percent"],
        alpha=0.20,
        label="Return P25–P75",
    )
    axes[0].set_ylabel("Net return (%)")
    axes[0].legend(fontsize=8)
    axes[0].grid(alpha=0.25)

    axes[1].plot(x, grouped["median_max_drawdown_percent"], marker="o", label="Median max DD")
    axes[1].plot(x, grouped["mean_max_drawdown_percent"], marker="x", label="Mean max DD")
    axes[1].plot(x, grouped["p75_max_drawdown_percent"], linestyle="--", label="P75 max DD")
    axes[1].set_ylabel("Maximum drawdown (%)")
    axes[1].legend(fontsize=8)
    axes[1].grid(alpha=0.25)

    axes[2].plot(x, grouped["median_calmar_ratio"], marker="o", label="Median Calmar")
    axes[2].plot(x, grouped["filter_pass_rate"] * 100.0, marker="s", label="Filter pass rate (%)")
    axes[2].set_xlabel(parameter)
    axes[2].set_ylabel("Calmar / pass rate")
    axes[2].legend(fontsize=8)
    axes[2].grid(alpha=0.25)

    figure.suptitle(
        f"{strategy}: marginal summary for {parameter}\n"
        "Mean is diagnostic; median and quantiles are the primary robustness view."
    )
    figure.tight_layout()
    figure.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(figure)
    return grouped


def table_html(frame: pd.DataFrame, max_rows: int = 25, decimals: int = 3) -> str:
    if frame.empty:
        return "<p class='muted'>No rows available.</p>"
    display = frame.head(max_rows).copy()
    for column in display.columns:
        if pd.api.types.is_float_dtype(display[column]):
            display[column] = display[column].map(
                lambda value: "" if pd.isna(value) else f"{value:.{decimals}f}"
            )
    return display.to_html(index=False, escape=True, classes="data-table", border=0)


def integrity_table(frame: pd.DataFrame, successful: pd.DataFrame, active_parameters: list[str]) -> pd.DataFrame:
    status_counts = frame["status"].value_counts(dropna=False).to_dict()
    duplicate_count = 0
    if active_parameters:
        duplicate_count = int(
            frame.dropna(subset=active_parameters).duplicated(active_parameters).sum()
        )
    finite_success = int(finite_metric_mask(successful).sum()) if not successful.empty else 0
    invalid_numeric = int(len(successful) - finite_success)
    zero_trade = int((successful["trade_count"] == 0).sum()) if not successful.empty else 0
    return pd.DataFrame(
        [
            ("Rows in CSV", len(frame)),
            ("Successful rows", int(status_counts.get("success", 0))),
            ("Invalid rows", int(status_counts.get("invalid", 0))),
            ("Failed rows", int(status_counts.get("failed", 0))),
            ("Other / unknown status rows", int(len(frame) - sum(status_counts.get(k, 0) for k in ["success", "invalid", "failed"]))),
            ("Duplicate parameter combinations", duplicate_count),
            ("Successful rows with non-finite required metrics", invalid_numeric),
            ("Successful zero-trade rows", zero_trade),
        ],
        columns=["check", "value"],
    )


def choose_verdict(summary: dict[str, Any], threshold_config: dict[str, Any]) -> tuple[str, str]:
    successful = int(summary.get("active_successful_runs", summary["successful_runs"]))
    pass_rate = float(summary["filter_pass_rate"])
    robust = int(summary["robust_candidate_count"])

    if successful < int(threshold_config["minimum_successful_runs"]):
        return "INSUFFICIENT DATA", "Too few successful combinations for a meaningful in-sample surface."
    if robust >= int(threshold_config["minimum_robust_candidates"]) and pass_rate >= float(threshold_config["minimum_filter_pass_rate"]):
        return "ADVANCE TO COST / SLIPPAGE STRESS TEST", "Several neighbouring parameter combinations pass the in-sample filters."
    if pass_rate > 0.0:
        return "WATCH / REVIEW", "There are passing combinations, but robustness is too thin for the next gate."
    return "DO NOT ADVANCE YET", "No successful configuration passed the current in-sample filters."


def process_strategy(
    csv_path: Path,
    parameter_grid: pd.DataFrame | None,
    output_dir: Path,
    config: dict[str, Any],
) -> StrategyResult:
    raw = normalise_frame(read_csv_or_raise(csv_path))
    strategy_values = raw["strategy"].dropna().astype(str).unique().tolist()
    strategy = strategy_values[0] if strategy_values else csv_path.stem
    if len(strategy_values) > 1:
        raise ValueError(f"{csv_path.name} contains more than one strategy: {strategy_values}")

    active_parameters = enabled_parameters_from_grid(parameter_grid, strategy, raw)
    local_config = strategy_config(config, strategy)

    successful_raw = raw[raw["status"].eq("success")].copy()
    successful_all = successful_raw[finite_metric_mask(successful_raw)].copy()
    integrity = integrity_table(raw, successful_raw, active_parameters)

    if successful_all.empty:
        raise ValueError(f"{strategy}: there are no successful runs with finite core metrics.")

    zero_trade_successful_count = int((successful_all["trade_count"] == 0).sum())

    # Main analysis excludes zero-trade rows.
    # Zero-trade rows are still counted in integrity checks and summary.
    successful = successful_all[successful_all["trade_count"] > 0].copy()

    if successful.empty:
        raise ValueError(f"{strategy}: there are no successful runs with trade_count > 0.")

    successful = apply_filters(successful, local_config["filters"])

    # Pareto should not include tiny-sample trades.
    # Use the same minimum trade-count threshold as the filters.
    successful["is_pareto"] = False
    min_trade_count = float(local_config["filters"]["min_trade_count"])
    pareto_base = successful[successful["trade_count"] >= min_trade_count].copy()

    if not pareto_base.empty:
        successful.loc[pareto_base.index, "is_pareto"] = pareto_frontier_mask(pareto_base)

    successful = add_neighbourhood_metrics(successful, active_parameters, local_config)

    strategy_output = output_dir / safe_filename(strategy)
    strategy_output.mkdir(parents=True, exist_ok=True)

    integrity.to_csv(strategy_output / "integrity_checks.csv", index=False)
    successful.to_csv(strategy_output / "successful_runs_enriched.csv", index=False)

    pareto = successful[successful["is_pareto"]].sort_values(
        ["max_drawdown_percent", "net_return_percent"], ascending=[True, False]
    )
    pareto.to_csv(strategy_output / "pareto_frontier.csv", index=False)

    ranked = successful[successful["passes_filters"]].copy()
    ranked = ranked.sort_values(
        [
            "is_robust_candidate",
            "neighbour_pass_rate",
            "calmar_ratio",
            "max_drawdown_percent",
            "net_return_percent",
        ],
        ascending=[False, False, False, True, False],
        kind="mergesort",
    )
    ranked.insert(0, "candidate_rank", np.arange(1, len(ranked) + 1))
    top_candidates = ranked.head(int(local_config["robustness"]["top_candidates_to_show"])).copy()
    top_candidates.to_csv(strategy_output / "top_candidates.csv", index=False)

    save_distribution_chart(successful, strategy_output / "distributions.png", strategy)
    save_return_drawdown_chart(successful, strategy_output / "return_vs_drawdown.png", strategy)

    marginal_sections: list[str] = []
    for parameter in active_parameters:
        marginal = save_marginal_chart(
            successful,
            parameter,
            strategy_output / f"marginal_{safe_filename(parameter)}.png",
            strategy,
        )
        marginal.to_csv(strategy_output / f"marginal_{safe_filename(parameter)}.csv", index=False)
        marginal_sections.append(
            f"<section><h3>Marginal view: {html.escape(parameter)}</h3>"
            f"<img src='{safe_filename(strategy)}/marginal_{safe_filename(parameter)}.png' alt='Marginal chart for {html.escape(parameter)}'>"
            f"</section>"
        )

    heatmap_sections: list[str] = []
    if len(active_parameters) >= 2:
        for first_index in range(len(active_parameters)):
            for second_index in range(first_index + 1, len(active_parameters)):
                x_parameter = active_parameters[first_index]
                y_parameter = active_parameters[second_index]
                pair_name = f"{safe_filename(x_parameter)}__{safe_filename(y_parameter)}"
                pair_data = save_pair_heatmap(
                    successful,
                    x_parameter,
                    y_parameter,
                    strategy_output / f"heatmap_{pair_name}.png",
                    strategy,
                )
                pair_data.to_csv(strategy_output / f"heatmap_{pair_name}.csv", index=False)
                heatmap_sections.append(
                    f"<section><h3>Interaction: {html.escape(x_parameter)} × {html.escape(y_parameter)}</h3>"
                    f"<img src='{safe_filename(strategy)}/heatmap_{pair_name}.png' alt='Heatmaps for {html.escape(x_parameter)} and {html.escape(y_parameter)}'>"
                    f"</section>"
                )

    metric_summary: dict[str, Any] = {}
    for metric in ["net_return_percent", "max_drawdown_percent", "calmar_ratio", "profit_factor", "trade_count"]:
        for key, value in describe_metric(successful[metric]).items():
            metric_summary[f"{metric}_{key}"] = value

    summary_row: dict[str, Any] = {
        "strategy": strategy,
        "source_csv": csv_path.name,
        "active_parameters": ", ".join(active_parameters) if active_parameters else "(none detected)",
        "total_rows": int(len(raw)),
        "successful_runs": int(len(successful_raw)),
        "finite_successful_runs": int(len(successful_all)),
        "active_successful_runs": int(len(successful)),
        "invalid_runs": int((raw["status"] == "invalid").sum()),
        "failed_runs": int((raw["status"] == "failed").sum()),
        "duplicate_parameter_combinations": int(integrity.loc[integrity["check"] == "Duplicate parameter combinations", "value"].iloc[0]),
        "zero_trade_successful_runs": zero_trade_successful_count,
        "filter_pass_count": int(successful["passes_filters"].sum()),
        "filter_pass_rate": float(successful["passes_filters"].mean()),
        "robust_candidate_count": int(successful["is_robust_candidate"].sum()),
        "pareto_frontier_count": int(successful["is_pareto"].sum()),
        **metric_summary,
    }
    verdict, verdict_reason = choose_verdict(summary_row, local_config["verdict"])
    summary_row["in_sample_verdict"] = verdict
    summary_row["verdict_reason"] = verdict_reason

    filters_table = pd.DataFrame(
        [(key, value) for key, value in local_config["filters"].items()],
        columns=["filter", "value"],
    )
    robustness_table = pd.DataFrame(
        [(key, value) for key, value in local_config["robustness"].items()],
        columns=["robustness_rule", "value"],
    )

    report_html = f"""
    <section class='strategy-card'>
      <h2 id='{safe_filename(strategy)}'>{html.escape(strategy)}</h2>
      <p><span class='verdict'>{html.escape(verdict)}</span> {html.escape(verdict_reason)}</p>
      <p class='muted'>This is an in-sample triage report. It does not establish live tradability, cost robustness, or out-of-sample validity.</p>
      <h3>Study and filter summary</h3>
      {table_html(pd.DataFrame([summary_row]).drop(columns=['source_csv', 'verdict_reason']), max_rows=1)}
      <div class='split'>
        <div><h3>Integrity checks</h3>{table_html(integrity, max_rows=20)}</div>
        <div><h3>Current filters</h3>{table_html(filters_table, max_rows=20)}<h3>Robustness rule</h3>{table_html(robustness_table, max_rows=20)}</div>
      </div>
      <h3>Distribution and return–drawdown view</h3>
      <div class='image-grid'>
        <img src='{safe_filename(strategy)}/distributions.png' alt='Metric distributions for {html.escape(strategy)}'>
        <img src='{safe_filename(strategy)}/return_vs_drawdown.png' alt='Return versus drawdown for {html.escape(strategy)}'>
      </div>
      <h3>Top filtered candidates</h3>
      <p class='muted'>Ranked first by robust-neighbour status and neighbour pass rate, then Calmar, drawdown and return. This is not an automated parameter selection.</p>
      {table_html(top_candidates, max_rows=int(local_config['robustness']['top_candidates_to_show']))}
      <h3>Pareto frontier: return versus maximum drawdown</h3>
      {table_html(pareto, max_rows=25)}
      <h3>Marginal diagnostics</h3>
      <p class='muted'>Mean is included to detect outlier-driven results. Median and quantiles remain the primary marginal robustness view.</p>
      {''.join(marginal_sections) if marginal_sections else '<p class="muted">No active varying parameters were detected.</p>'}
      <h3>Parameter-interaction heatmaps</h3>
      {''.join(heatmap_sections) if heatmap_sections else '<p class="muted">At least two varying parameters are required for pairwise heatmaps.</p>'}
      <p><a href='{safe_filename(strategy)}/successful_runs_enriched.csv'>Download enriched successful-run data</a> ·
         <a href='{safe_filename(strategy)}/top_candidates.csv'>Download top candidates</a> ·
         <a href='{safe_filename(strategy)}/pareto_frontier.csv'>Download Pareto frontier</a></p>
    </section>
    """

    return StrategyResult(
        strategy=strategy,
        active_parameters=active_parameters,
        output_dir=strategy_output,
        summary_row=summary_row,
        report_html=report_html,
    )


def build_index_html(
    study_dir: Path,
    output_dir: Path,
    results: list[StrategyResult],
    metadata: pd.DataFrame | None,
) -> str:
    summary = pd.DataFrame([result.summary_row for result in results])
    study_metadata_html = "<p class='muted'>study_metadata.csv was not found.</p>"
    if metadata is not None and not metadata.empty:
        study_metadata_html = table_html(metadata, max_rows=20)

    generated = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    strategy_sections = "\n".join(result.report_html for result in results)

    return f"""<!DOCTYPE html>
<html lang='en'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Sensitivity analysis report</title>
  <style>
    :root {{ --ink:#172033; --muted:#64748b; --line:#dbe3ee; --page:#f6f8fb; --card:#fff; --accent:#2059b7; }}
    * {{ box-sizing:border-box; }}
    body {{ margin:0; font-family:Inter,Segoe UI,Arial,sans-serif; background:var(--page); color:var(--ink); line-height:1.45; }}
    main {{ max-width:1500px; margin:0 auto; padding:30px 22px 60px; }}
    h1 {{ margin:0 0 6px; letter-spacing:-0.03em; }}
    h2 {{ margin-top:0; }}
    h3 {{ margin:22px 0 10px; }}
    .muted {{ color:var(--muted); }}
    .card, .strategy-card {{ background:var(--card); border:1px solid var(--line); border-radius:12px; padding:20px; margin-top:18px; box-shadow:0 1px 2px rgba(20,35,60,.04); }}
    .verdict {{ font-weight:700; color:var(--accent); }}
    .data-table {{ border-collapse:collapse; width:100%; font-size:12px; margin:8px 0 18px; overflow:auto; display:block; }}
    .data-table th, .data-table td {{ border:1px solid var(--line); padding:7px 8px; text-align:right; white-space:nowrap; }}
    .data-table th {{ background:#f8fafc; color:var(--muted); font-size:11px; text-transform:uppercase; letter-spacing:.03em; }}
    .data-table th:first-child, .data-table td:first-child {{ text-align:left; }}
    .split {{ display:grid; grid-template-columns:1fr 1fr; gap:18px; }}
    .image-grid {{ display:grid; grid-template-columns:1fr 1fr; gap:18px; }}
    img {{ max-width:100%; height:auto; background:#fff; border:1px solid var(--line); border-radius:8px; }}
    a {{ color:var(--accent); }}
    @media(max-width:950px) {{ .split,.image-grid {{ grid-template-columns:1fr; }} main {{ padding:20px 12px 40px; }} }}
  </style>
</head>
<body>
<main>
  <header>
    <h1>In-sample parameter-sensitivity analysis</h1>
    <p class='muted'>Source study: {html.escape(str(study_dir))}<br>Generated: {generated}</p>
  </header>

  <section class='card'>
    <h2>How to read this report</h2>
    <p>Use the CSV-level report to identify broad, neighbouring regions that combine acceptable return, drawdown, trade count, Calmar and profit factor. Do not choose the single highest historical return. The mean is shown only as an outlier diagnostic; medians, quantiles, pass rates and parameter neighbourhoods are the primary robustness checks.</p>
    <p>This report is deliberately limited to in-sample parameter analysis. Passing it means only that a strategy deserves the next gates: realistic fees/slippage, chronological out-of-sample validation and portfolio analysis.</p>
  </section>

  <section class='card'>
    <h2>Cross-strategy summary</h2>
    {table_html(summary, max_rows=50)}
    <p><a href='summary.csv'>Download summary.csv</a> · <a href='analysis_config_used.json'>Download the configuration used</a></p>
  </section>

  <section class='card'>
    <h2>Study metadata</h2>
    {study_metadata_html}
  </section>

  {strategy_sections}
</main>
</body>
</html>
"""


def main() -> int:
    args = parse_args()
    study_dir = args.study_dir.expanduser().resolve()
    if not study_dir.is_dir():
        print(f"ERROR: study directory does not exist: {study_dir}", file=sys.stderr)
        return 2

    output_dir = (args.output_dir or (study_dir / "analysis_report")).expanduser().resolve()
    if output_dir.exists() and any(output_dir.iterdir()) and not args.overwrite:
        print(
            f"ERROR: output directory already contains files: {output_dir}\n"
            "Use --overwrite or choose a different --output-dir.",
            file=sys.stderr,
        )
        return 2
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        config = load_config(args.config)
        with (output_dir / "analysis_config_used.json").open("w", encoding="utf-8") as handle:
            json.dump(config, handle, indent=2)

        parameter_grid_path = study_dir / "parameter_grid.csv"
        parameter_grid = read_csv_or_raise(parameter_grid_path) if parameter_grid_path.is_file() else None

        metadata_path = study_dir / "study_metadata.csv"
        metadata = read_csv_or_raise(metadata_path) if metadata_path.is_file() else None

        strategy_paths = strategy_csv_paths(study_dir)
        if not strategy_paths:
            raise FileNotFoundError(
                "No strategy result CSV files found. Expected files such as XHBreakout.csv with status and run_id columns."
            )

        results: list[StrategyResult] = []
        for path in strategy_paths:
            print(f"Analysing {path.name}...")
            results.append(process_strategy(path, parameter_grid, output_dir, config))

        summary = pd.DataFrame([result.summary_row for result in results])
        summary.to_csv(output_dir / "summary.csv", index=False)
        index_html = build_index_html(study_dir, output_dir, results, metadata)
        (output_dir / "index.html").write_text(index_html, encoding="utf-8")

        print("\nAnalysis complete.")
        print(f"Open this file in a browser:\n  {output_dir / 'index.html'}")
        return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
