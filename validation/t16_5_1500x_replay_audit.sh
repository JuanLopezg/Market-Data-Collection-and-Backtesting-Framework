#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

echo "============================================================"
echo "T16.5 — 1500x EXPLORATORY REPLAY HARNESS AUDIT (WAL READER HOTFIX)"
echo "============================================================"

python3 tools/historical_replay/run_exploratory_1500x.py --root . --dry-run

grep -q -- '--init-only' live_trading/historical_market_data_service/src/historical_market_data_service_main.cpp
grep -q 'event=market_data_db_initialized' live_trading/historical_market_data_service/src/historical_market_data_service_main.cpp
grep -q '^  market-data-db-init:' deploy/historical_replay/docker-compose.yml
grep -q '"market-data-db-init"' tools/historical_replay/run_exploratory_1500x.py
grep -q '"restarting"' tools/historical_replay/run_exploratory_1500x.py

# Canonical consumers must remain logically read-only at SQLite API level.
grep -q 'SQLITE_OPEN_READONLY' lib/src/market/canonical_market_data_reader.cpp
grep -q 'PRAGMA query_only=ON' lib/src/market/canonical_market_data_reader.cpp

python3 - <<'PY'
from pathlib import Path
import re
text=Path('deploy/historical_replay/docker-compose.yml').read_text()

def block_for(service: str) -> str:
    m=re.search(rf'(?ms)^  {re.escape(service)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)', text)
    assert m, f'missing service block: {service}'
    return m.group(0)

for svc in ('strategy','portfolio-risk','execution-state'):
    block=block_for(svc)
    assert 'market-data-db-init:' in block, f'{svc} missing DB init dependency'
    assert 'market-data-db:/data/market:rw' in block, f'{svc} must mount canonical SQLite RW for WAL/SHM support'

feeder=block_for('historical-market-data')
assert 'market-data-db:/data/market:rw' in feeder, 'historical feeder must own writable canonical SQLite mount'
print('bootstrap-dependencies-ok')
print('wal-reader-mounts-ok')
PY

echo "PASS: canonical consumers use SQLite READONLY + query_only"
echo "PASS: filesystem mount permits SQLite WAL/SHM auxiliary files"
echo "PASS: DB init dependencies are structurally present"
echo "PASS: restart detection remains enabled"
echo "PASS: T16.5 WAL reader hotfix audit"

python3 - "$ROOT/tools/historical_replay/run_exploratory_1500x.py" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text(encoding="utf-8")
assert 'EXPECTED_ONE_SHOT_SERVICES' in s, 'runner missing one-shot service allowlist'
assert '"market-data-volume-init"' in s, 'volume init not allowlisted'
assert '"market-data-db-init"' in s, 'db init not allowlisted'
assert 'if "exited (0)" in low:' in s, 'runner does not permit successful one-shot exits'
assert '"restarting" in low' in s and '"unhealthy" in low' in s, 'runtime failure detection weakened'
print('PASS: successful init-only jobs are ignored while runtime failure detection remains strict')
PY

echo "PASS: T16.5 init-job detector hotfix audit"
