#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCOPE="${1:-full}"

case "$SCOPE" in
    quick|full) ;;
    *) echo "Usage: bash validation/shared_clock_acceptance_suite.sh [quick|full]" >&2; exit 2 ;;
esac

run() {
    echo
    echo "============================================================"
    echo "STEP 35G — $1"
    echo "============================================================"
    shift
    "$@"
}

echo "============================================================"
echo "STEP 35G — FINAL SHARED LOGICAL CLOCK ACCEPTANCE"
echo "scope : $SCOPE"
echo "root  : $ROOT"
echo "============================================================"

run "source architecture gate" python3 "$SCRIPT_DIR/shared_clock_acceptance_check.py"
run "clock audit" bash "$SCRIPT_DIR/clock_audit.sh"
run "clock foundation" bash "$SCRIPT_DIR/shared_clock_foundation_check.sh"

# Rebuild and prove the current post-controls source tree still has all eight
# participants synchronized in an actual distributed replay.
run "current-tree follower integration" bash "$SCRIPT_DIR/shared_clock_followers_suite.sh"

# 35F changed the replay authority, not follower bootstrap code. Re-run the hardest
# follower hard-restart case on the current image to prove sync/refresh behavior did not
# regress, then re-run both authority causal phases because the authority itself changed.
run "post-controls follower restart regression" \
    env CLOCK_RESYNC_FORCE_RUNTIME_BUILD=0 bash "$SCRIPT_DIR/shared_clock_restart_resync_suite.sh" market-data
run "post-controls authority restart regression" \
    env CLOCK_AUTHORITY_FORCE_RUNTIME_BUILD=0 bash "$SCRIPT_DIR/shared_clock_authority_restart_suite.sh"

# Full speed/pause economic invariance on the exact current source tree.
run "speed / pause / resume invariance" bash "$SCRIPT_DIR/shared_clock_controls_suite.sh"

# The controls suite rebuilt the runtime image from the current tree. Reuse that exact
# image to prove LIVE and TESTNET never instantiate or require the fake-clock control plane.
run "LIVE + TESTNET fake-clock isolation" \
    env CLOCK_ISOLATION_FORCE_RUNTIME_BUILD=0 bash "$SCRIPT_DIR/live_clock_isolation_suite.sh"

if [[ "$SCOPE" == "quick" ]]; then
    echo
    echo "============================================================"
    echo "STEP 35G QUICK PRECHECK: PASS"
    echo "This is not the closure marker. Dashboard development may proceed, but run 'full'"
    echo "before final dashboard/system acceptance to close shared-clock + full-history economics."
    echo "============================================================"
    exit 0
fi

# Re-run the pre-clock resilience gate after all clock wiring/control changes. This catches
# interactions with NATS/PostgreSQL outages, delivery redelivery, crash recovery and safety.
run "post-clock restart + chaos regression" bash "$SCRIPT_DIR/pre_dashboard_suite.sh"

# Final economic oracle on the exact current tree: both full-history portfolio modes,
# distributed == fast, and the locked RealTest accepted baseline.
run "full-history economic regression" bash "$SCRIPT_DIR/run_validation.sh"

echo
echo "============================================================"
echo "STEP 35G SHARED CLOCK ACCEPTANCE RESULT: PASS"
echo "Shared logical clock phase : CLOSED"
echo "LIVE/TESTNET isolation     : PASS"
echo "REPLAY shared time         : PASS"
echo "Follower restart/resync    : PASS"
echo "Authority restart/recovery : PASS"
echo "Pause/speed invariance     : PASS"
echo "Restart/chaos regression   : PASS"
echo "Full-history economics     : PASS"
echo "Next                       : STEP 36 — Trading Control Dashboard"
echo "============================================================"
