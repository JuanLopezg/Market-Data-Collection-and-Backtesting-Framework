# Sensitivity CSV analysis reports

The program reads completed parameter-sensitivity CSV studies. It does not rerun
backtests or establish fees/slippage/out-of-sample acceptance. A favorable in-sample
verdict means a candidate needs further testing.

The files already live directly in `tools/`:

| File | Purpose |
| --- | --- |
| `sensitivity_report_main.py` | Report program. |
| `sensitivity_report_config.json` | Analysis configuration. |
| `requirements_sensitivity_report.txt` | Python dependencies. |
| `verify_tool.py` | Program/configuration integrity check. |
| `TOOL_VERSION.txt` | Tool version metadata. |

## Run from the repository root under WSL

```bash
python3 tools/verify_tool.py
python3 -m pip install -r tools/requirements_sensitivity_report.txt
python3 tools/sensitivity_report_main.py \
  --study-dir storage/backtests/sensitivity_results/local_refinement_v2 \
  --config tools/sensitivity_report_config.json \
  --overwrite
```

Choose an existing completed study directory. `--overwrite` replaces its prior
analysis report. Open `analysis_report/index.html` inside that study directory.
`python3 tools/sensitivity_report_main.py --help` lists current options.

## Output and interpretation

The report writes HTML, summary/configuration CSV/JSON, per-strategy integrity checks,
enriched successful runs, candidate rankings, Pareto frontiers, distributions,
parameter marginals and pairwise heatmaps.

Checks cover failed/invalid/duplicate and zero-trade runs; return/drawdown/Calmar/trade
distributions; median/quantile robustness; nearest-neighbor robustness; and conservative
in-sample triage. Means are diagnostic, not proof of robustness.

Keep the configuration used with the report. Validate promising candidates through the
current replay workflow, realistic execution assumptions and out-of-sample studies.
Unmigrated research study generators still use the frozen research runtime; this report
tool analyzes their completed CSVs and does not replace their migration.
