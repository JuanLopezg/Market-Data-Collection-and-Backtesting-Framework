#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step54"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP54: PASS: %s\n' "$*"; }
fail(){ printf 'STEP54: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 54 — RECONCILIATION + LEDGER PARITY'
printf '%s\n' '============================================================'
printf '%s\n' 'MOCK local-expected vs venue truth. No private real-venue routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step52 + Step53 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step52/SHA256SUMS >/dev/null) \
  || fail 'Step52 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step53/SHA256SUMS >/dev/null) \
  || fail 'Step53 frozen artifact changed'
pass 'Step52 accounting and Step53 recovery remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step54 artifacts are present and hash-clean'
for f in \
 "$CFG/reconciliation_ledger_v1.json" \
 "$CFG/reconciliation_ledger_manifest_v1.json" \
 "$ART/STEP_54_RECONCILIATION_LEDGER_PARITY.md" \
 "$ART/STEP_55_IMPLEMENTATION_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_sha256_v1.h" \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 "$ROOT/validation/step54_mock_reconciliation_ledger_parity_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) \
  || fail 'Step54 hash mismatch'
pass 'Step54 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step54 fingerprint binds to exact Step53 recovery fingerprint'
python3 -S - "$CFG/snapshot_recovery_manifest_v1.json" "$CFG/reconciliation_ledger_v1.json" "$CFG/reconciliation_ledger_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s53=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step54-v1\n{s53['combinedRecoveryFingerprint']}\n"
    f"mock-reconciliation-ledger-v1-step54\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep53RecoveryFingerprint']==s53['combinedRecoveryFingerprint']
assert m['step53RecoveryFingerprint']==s53['combinedRecoveryFingerprint']
assert m['reconciliationConfigSha256']==sha
assert m['combinedReconciliationLedgerFingerprint']==fp
assert m['cleanBlockedPendingImplemented'] is True
assert m['cashPositionOrderFillParityImplemented'] is True
assert m['sha256LedgerChainImplemented'] is True
assert m['ledgerCashPositionParityImplemented'] is True
assert m['newOrderReconciliationGateImplemented'] is True
assert m['restartParityImplemented'] is True
assert m['realVenueReconciliationImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_55_FAULT_CHAOS_RATE_LIMIT_ENGINE'
PY
pass 'reconciliation/ledger fingerprint is reproducible and bound to Step53'

printf '%s\n' '[4/8] CLEAN/BLOCKED/PENDING, ledger and routing semantics are explicit/fail-closed'
python3 -S - "$CFG/reconciliation_ledger_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['states']==['CLEAN','BLOCKED','PENDING']
assert d['freshnessPolicy']['sequenceMismatch']=='PENDING'
assert d['freshnessPolicy']['missingVenueEvidence']=='PENDING'
assert d['freshnessPolicy']['recoveryUnsafe']=='BLOCKED'
assert d['dimensions']['cash']=='SETTLED_CASH_BALANCE_TOTAL_EXACT_FIXED_POINT'
assert d['ledger']['hashChain']=='SHA256'
assert d['ledger']['exactlyOneTradingFeePerFill'] is True
assert d['ledger']['finalCashMustMatchVenueSettledCash'] is True
assert d['ledger']['finalPositionsMustMatchVenuePositions'] is True
assert d['ledger']['fillSetMustMatchVenueFillSnapshot'] is True
assert d['routingGate']['blockedOrPending']=='NO_NEW_SUBMIT'
assert d['routingGate']['stateAdvanceAfterClean']=='PENDING_UNTIL_RECONCILED_AGAIN'
assert d['wallClockUsedForEconomicsOrReconciliation'] is False
assert d['realVenueReconciliationImplemented'] is False
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'freshness cannot fake CLEAN; contradictions BLOCK and unsafe/stale state blocks new submits'

printf '%s\n' '[5/8] C++17 reconciliation/ledger/restart/routing suite passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" \
 -I"$ROOT/lib/src/exchange" \
 -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step54_mock_reconciliation_ledger_parity_test.cpp" \
 -o "$TMP/step54_test"
"$TMP/step54_test"
pass 'cash/positions/open-orders/fills, SHA-256 chain, restart parity and route blocking validated'

printf '%s\n' '[6/8] Ledger chain and routing guard are present; no wall-clock reconciliation'
grep -Fq 'Sha256V1::hexDigest' \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 || fail 'SHA-256 ledger chain call missing'
grep -Fq 'canRouteNewOrders' \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 || fail 'new-order reconciliation guard missing'
grep -Fq 'submitIfSafe' \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 || fail 'guarded submit boundary missing'
if grep -Ein \
 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|gettimeofday|clock_gettime|std::time|(^|[^[:alnum:]_])time\(' \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 "$ROOT/lib/src/exchange/mock_sha256_v1.h" >"$TMP/clock"; then
  cat "$TMP/clock" >&2
  fail 'wall/monotonic clock primitive leaked into Step54 reconciliation economics'
fi
pass 'ledger/routing safety is explicit and reconciliation uses supplied evidence only'

printf '%s\n' '[7/8] Step55/private real-venue implementation has not slipped in'
if find "$ROOT/lib/src/exchange" -maxdepth 1 -type f \
   \( -iname '*chaos*' -o -iname '*fault*engine*' -o -iname '*rate_limit*engine*' \) \
   -print -quit | grep -q .; then
  fail 'Step55 fault/chaos engine detected during Step54'
fi
if grep -Ein \
 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|smart[_ -]?order|split[_ -]?routing' \
 "$ROOT/lib/src/exchange/mock_reconciliation_ledger_parity_v1.h" \
 "$ROOT/lib/src/exchange/mock_sha256_v1.h" >"$TMP/future"; then
  cat "$TMP/future" >&2
  fail 'private real-venue/smart-routing concern leaked into Step54 code'
fi
pass 'Step54 stays MOCK-only and does not enable private venue or smart routing'

printf '%s\n' '[8/8] Step55 handoff is bound to exact Step54 fingerprint'
python3 -S - "$CFG/reconciliation_ledger_manifest_v1.json" "$ART/STEP_55_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step54-to-step55-v1'
assert h['nextStep']=='STEP_55_FAULT_CHAOS_RATE_LIMIT_ENGINE'
assert h['step54ReconciliationLedgerFingerprint']==m['combinedReconciliationLedgerFingerprint']
assert h['step53RecoveryFingerprint']==m['step53RecoveryFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert 'unsafe or stale reconciliation blocks new submits' in h['requiredStep55Behavior']
PY
pass 'next safe step is Step55 Fault / Chaos / Rate-Limit Engine'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 54: PASS — RECONCILIATION + LEDGER PARITY VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'CLEAN/BLOCKED/PENDING reconciliation and SHA-256 fill/accounting ledger parity are validated.'
printf '%s\n' 'Unsafe or stale reconciliation blocks new submits. No private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 55 Fault / Chaos / Rate-Limit Engine.'
