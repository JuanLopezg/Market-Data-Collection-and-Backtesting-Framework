# Sensitivity CSV analysis report tool (v3)

This folder analyses **completed parameter-sensitivity CSV studies**. It does not rerun backtests, apply fees/slippage, or perform out-of-sample validation.

## Important: keep this folder together

Do **not** copy files individually into `tools/`. Extract or copy the entire folder as:

```text
tools/sensitivity_report_tool_v3/
```

The four core files must remain distinct:

```text
sensitivity_report_main.py          Python program
sensitivity_report_config.json      JSON configuration
requirements_sensitivity_report.txt Python dependencies
verify_tool.py                      checks that the files were not mixed
```

## Install and run from WSL

Assuming you are in your project's `build/` directory:

```bash
# One-time: verify the folder has not been mixed up
python3 ../tools/sensitivity_report_tool_v3/verify_tool.py

# One-time: install Python packages
python3 -m pip install -r ../tools/sensitivity_report_tool_v3/requirements_sensitivity_report.txt

# Create / overwrite the report
python3 ../tools/sensitivity_report_tool_v3/sensitivity_report_main.py \
  --study-dir ../storage/backtests/sensitivity_results/local_refinement_v2 \
  --config ../tools/sensitivity_report_tool_v3/sensitivity_report_config.json \
  --overwrite
```

When it finishes, open:

```text
../storage/backtests/sensitivity_results/local_refinement_v2/analysis_report/index.html
```

From WSL:

```bash
explorer.exe "$(wslpath -w ../storage/backtests/sensitivity_results/local_refinement_v2/analysis_report/index.html)"
```

## Output

The report creates:

```text
analysis_report/
├── index.html
├── summary.csv
├── analysis_config_used.json
├── <Strategy>/
│   ├── integrity_checks.csv
│   ├── successful_runs_enriched.csv
│   ├── top_candidates.csv
│   ├── pareto_frontier.csv
│   ├── distributions.png
│   ├── return_vs_drawdown.png
│   ├── marginal_<parameter>.png/.csv
│   └── heatmap_<parameter_A>__<parameter_B>.png/.csv
```

## What the report checks

- row counts, failed/invalid results, duplicates and zero-trade combinations;
- distributions of return, drawdown, Calmar and trade count;
- mean, median and P25–P75 statistics (mean is diagnostic; median/quantiles are robustness signals);
- pairwise heatmaps for return, drawdown, Calmar and pass rate;
- return–drawdown Pareto frontier;
- nearest-neighbour robustness;
- a conservative in-sample triage verdict.

A favourable verdict only means that the strategy is worth testing next under fees/slippage and out-of-sample data.
