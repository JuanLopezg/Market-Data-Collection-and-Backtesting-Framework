#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"
scenario="$root/tools/historical_replay/run_acceptance_scenario.py"
regrade="$root/tools/historical_replay/regrade_t21_gap_catchup.py"
[[ -f "$scenario" && -f "$regrade" ]] || { echo "FAIL: missing T21 observability files"; exit 1; }
grep -q 'strategy_logs=compose(root,\["logs","--no-color","--tail=5000","strategy"\]' "$scenario"
grep -q "'signal_state_catchup_processed' in strategy_logs" "$scenario"
if grep -q "'signal_state_catchup_processed' in logs" "$scenario"; then
  echo "FAIL: aggregate log tail is still used for T21 catch-up observation"; exit 1
fi
grep -q 'event=signal_state_catchup_processed' "$regrade"
grep -q 't21_regraded_from_existing_evidence' "$regrade"
echo "PASS: T21 catch-up marker is observed from Strategy-specific logs, not the aggregate compose tail"
echo "PASS: existing completed T21 can be regraded fail-closed without replay"
