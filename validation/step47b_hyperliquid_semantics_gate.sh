#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 validation/step47b_hyperliquid_semantics_gate.py
