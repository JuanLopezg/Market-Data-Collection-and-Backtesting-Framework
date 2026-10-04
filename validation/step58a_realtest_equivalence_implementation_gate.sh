#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/historical_replay/step58a_realtest_equivalence_v1.json"
DOC="$ROOT/docs/venue/step58a/STEP_58A_1500X_FULL_HISTORY_REALTEST_EQUIVALENCE.md"
CANONICAL_GATE="$ROOT/validation/step58a_canonical_replay_suite_gate.sh"

pass(){ printf 'STEP58A-COMPAT: PASS: %s\n' "$*"; }
fail(){ printf 'STEP58A-COMPAT: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 58A — CANONICAL DIRECT REALTEST COMPATIBILITY GATE'
printf '%s\n' '============================================================'
printf '%s\n' 'This legacy entry point now validates the active canonical replay contract.'
printf '\n'

[[ -f "$CFG" ]] || fail 'missing canonical Step58A config'
[[ -f "$DOC" ]] || fail 'missing Step58A documentation'
[[ -x "$CANONICAL_GATE" || -f "$CANONICAL_GATE" ]] || fail 'missing canonical Step58A suite gate'

python3 -S - "$CFG" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1])
d=json.loads(p.read_text())
assert d['schemaVersion'] == 2
assert d['canonicalCli'] == 'python3 research/replay.py {fast|system|dashboard}'
h=d['historicalDataset']
assert h['start']=='2020-01-01' and h['end']=='2025-10-13'
assert h['marketDataPolicy']=='ORIGINAL_OHLCV_UNMODIFIED'
f=d['fullHistoryRealTest']
assert f['profile']=='realtest-parity'
assert f['realTestCsv']=='storage/backtests/final_tests/pureRSI.csv'
assert f['comparisonPolicy']=='DIRECT_REALTEST_MANUAL_REVIEW_V1'
assert f['hardcodedMismatchExceptions'] is False
assert f['historicalVolumeModified'] is False
assert f['executionCapacityUsesHistoricalVolume'] is False
assert f['matcherMaxAdverseSlippagePpm']==0
assert f['matcherFeePpm']==0 and f['accountingFeePpm']==0
assert f['normalPartialFillsEnabled'] is False
assert f['expectedCandidateTrades']==631
assert f['expectedFullyMatchedTrades']==628
assert f['expectedManualReviewDifferences']==4
assert f['expectedCanonicalFills']==1261
assert f['acceptedFullRunFingerprint']=='1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0'
r=d['restartResume']
assert set(r['supportedModes'])=={'system','dashboard'}
assert r['validated107DayFingerprint']=='94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2'
assert d['dashboardParity']['sameEconomicsAsSystem'] is True
assert d['dashboardParity']['fullHistoryFillsMustBeByteIdentical'] is True
assert all(value is False for value in d['safety'].values())
PY
pass 'active Step58A config describes direct RealTest parity, zero execution frictions and system/dashboard restart support'

if grep -Eq '0\.1\*v|10000000000|100000000000|v_prime=' "$CFG" "$DOC" "$ROOT/research/src/canonical_replay.cpp"; then
  fail 'obsolete synthetic-volume parity semantics remain in active Step58A files'
fi
pass 'active Step58A files contain no synthetic-volume parity transform'

if grep -Eq 'mockAccountingFeePpm[^0-9]*400|charges 400ppm|fee separation' "$CFG" "$DOC"; then
  fail 'obsolete fee-charging RealTest parity semantics remain in active Step58A files'
fi
pass 'RealTest parity documentation/config no longer claim positive MOCK fees'

printf '\n%s\n' 'Delegating behavioral proof to validation/step58a_canonical_replay_suite_gate.sh'
(cd "$ROOT" && bash validation/step58a_canonical_replay_suite_gate.sh)
