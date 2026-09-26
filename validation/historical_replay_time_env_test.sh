#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
python3 "$ROOT/validation/historical_replay_time_env_test.py" "$ROOT"
