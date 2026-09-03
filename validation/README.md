# Unified validation

Run the complete current economic regression suite from project root:

```bash
bash validation/run_validation.sh
```

The single script performs, in order:

1. PureRSI + EqualWeight full-history distributed replay.
2. Exact distributed-vs-fast comparison.
3. Exact `research/src/realtest.cpp` comparison against `pureRSI.csv`.
4. Locked known RealTest baseline check (only the confirmed BNB/FET/ZEC exceptions).
5. PureRSI + VolTarget full-history distributed replay.
6. Exact distributed-vs-fast comparison.
7. Exact research RealTest campaign comparison.
8. Locked known VolTarget RealTest baseline check.
9. PostgreSQL and long-lived service health gates for both runs.

The historical source defaults to `storage/databases/1d_cmc.csv` and the external RealTest source defaults to `storage/backtests/final_tests/pureRSI.csv`.

Environment overrides remain available:

- `HISTORICAL_DATA_PATH=/path/to/1d_cmc.csv`
- `REALTEST_CSV_PATH=/path/to/pureRSI.csv`
- `HISTORICAL_WARMUP_DAYS=30`
- `HISTORICAL_KEEP_ON_FAILURE=0|1` (defaults to `1` in this master validation)

The master validation sets `REALTEST_NONINTERACTIVE=1`, which only skips the manual ENTER prompts for mismatches. The matching and validation policy remains the exact research implementation. Normal research runs stay interactive.

Logs are retained under:

- `validation/logs/equal_weight/<timestamp>/`
- `validation/logs/vol_target/<timestamp>/`
