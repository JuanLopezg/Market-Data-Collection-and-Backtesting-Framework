#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

echo "============================================================"
echo "CURRENT SOURCE NO-LEGACY-CLOCK AUDIT"
echo "============================================================"

for p in \
  lib/src/contracts/clock_state.h \
  lib/src/contracts/clock_control.h \
  lib/src/contracts/clock_sync_request.h \
  lib/src/runtime/clock.h \
  lib/src/runtime/service_clock.h \
  lib/src/runtime/runtime_mode.h \
  live_trading/replay_controller \
  deploy/distributed_replay \
  tools/distributed_compare; do
  if [[ -e "$p" ]]; then
    echo "FAIL: legacy path still exists: $p" >&2
    exit 1
  fi
done

if grep -RInE \
  'ClockState|ClockControl|ClockSyncRequest|ServiceClockContext|SimulatedClock|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST|simulation\.clock\.(state|control|sync)|service_clock\.h|runtime_mode\.h|clock_state\.h|clock_control\.h|clock_sync_request\.h' \
  lib live_trading \
  --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' --include='*.cpp' --include='*.cxx' --include='meson.build' \
  > /tmp/algotrading_no_legacy_clock_hits.txt 2>/dev/null; then
  cat /tmp/algotrading_no_legacy_clock_hits.txt >&2
  echo "FAIL: legacy shared-clock source references remain" >&2
  exit 1
fi

grep -Fq 'return tradingRuntimeSubjects();' lib/src/transport/message_subjects.h

if grep -Eq "subdir\(['\"]replay_controller['\"]\)" live_trading/meson.build; then
  echo "FAIL: replay_controller remains in Meson graph" >&2
  exit 1
fi

if [[ -d live_trading/historical_market_data_service ]]; then
  grep -Eq "subdir\(['\"]historical_market_data_service/src['\"]\)" live_trading/meson.build || {
    echo "FAIL: historical_market_data_service directory exists but is absent from Meson graph" >&2
    exit 1
  }
fi


echo "PASS: no legacy shared-clock code/deploy/tooling remains"
