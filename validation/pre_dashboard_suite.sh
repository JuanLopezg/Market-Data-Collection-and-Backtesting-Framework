#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "============================================================"
echo "PRE-DASHBOARD INFRA VALIDATION"
echo "1/2 STEP 33E — all restart paths"
echo "2/2 STEP 34  — chaos/failure suite"
echo "============================================================"

bash "$SCRIPT_DIR/restart_suite.sh" all

# STEP 33E rebuilt the current runtime image already.
export CHAOS_SKIP_RUNTIME_BUILD=1
bash "$SCRIPT_DIR/chaos_suite.sh" all

echo
echo "============================================================"
echo "PRE-DASHBOARD INFRA VALIDATION: PASS"
echo "Restartability : PASS"
echo "Chaos/failures : PASS"
echo "Next           : STEP 35 Trading Control Dashboard"
echo "============================================================"
