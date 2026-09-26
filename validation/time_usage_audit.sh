#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
cd "$ROOT"

need_rg() {
  if ! command -v rg >/dev/null 2>&1; then
    echo "ERROR: ripgrep (rg) is required" >&2
    exit 2
  fi
}
need_rg

paths=(lib live_trading research tools deploy config)
existing=()
for p in "${paths[@]}"; do
  [[ -e "$p" ]] && existing+=("$p")
done

if [[ ${#existing[@]} -eq 0 ]]; then
  echo "ERROR: run from the algoTrading repository root" >&2
  exit 2
fi

echo "============================================================"
echo "T1 — TIME USAGE AUDIT (READ-ONLY)"
echo "============================================================"
echo

echo "[A] DIRECT REAL-TIME SOURCES / SLEEPS"
rg -n --hidden --glob '!**/__pycache__/**' --glob '*.{h,hpp,cpp,cc,cxx}' \
  '(system_clock::now\(|steady_clock::now\(|high_resolution_clock|std::time\(|time\(nullptr\)|sleep_for\(|sleep_until\(|computeNextMidnightUTC\(\)|getCurrentUtcDate\(\))' \
  "${existing[@]}" || true

echo
echo "[B] LEGACY CLOCK CONTROL PLANE"
rg -n --hidden --glob '!**/__pycache__/**' \
  '(ServiceClockContext|SimulatedClock|SystemClock|FixedClock|ClockState|ClockControl|ClockSyncRequest|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST|simulation\.clock\.|runtime_mode|simulation_id|--runtime-mode|--simulation-id)' \
  "${existing[@]}" || true

echo
echo "[C] TIME/DATE HELPERS THAT MAY HIDE HOST NOW()"
rg -n --hidden --glob '!**/__pycache__/**' --glob '*.{h,hpp,cpp,cc,cxx}' \
  '(newestCompletedUtcDate|currentUtcTimestamp|timeUntilUtcMidnight|getCurrentUtcDate|computeNextMidnightUTC|nowString)' \
  "${existing[@]}" || true

echo
echo "[D] LEGACY CLOCK FILES PRESENT"
legacy_files=(
  lib/src/runtime/clock.h
  lib/src/runtime/service_clock.h
  lib/src/runtime/runtime_mode.h
  lib/src/contracts/clock_state.h
  lib/src/contracts/clock_control.h
  lib/src/contracts/clock_sync_request.h
  live_trading/replay_controller/src/replay_controller_main.cpp
)
for f in "${legacy_files[@]}"; do
  if [[ -e "$f" ]]; then
    echo "FOUND  $f"
  else
    echo "ABSENT $f"
  fi
done

echo
echo "Audit complete. This script intentionally does not modify the repository."
