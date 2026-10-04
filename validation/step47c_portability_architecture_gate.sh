#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 validation/step47c_portability_architecture_gate.py
