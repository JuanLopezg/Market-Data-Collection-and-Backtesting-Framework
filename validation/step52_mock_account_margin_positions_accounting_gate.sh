#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step52"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP52: PASS: %s\n' "$*"; }
fail(){ printf 'STEP52: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 52 — MOCK ACCOUNT / MARGIN / POSITIONS / ACCOUNTING'
printf '%s\n' '============================================================'
printf '%s\n' 'Fill-driven economics only. No restart recovery/real reconciliation/private routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step50 + Step51 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step50/SHA256SUMS >/dev/null) || fail 'Step50 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step51/SHA256SUMS >/dev/null) || fail 'Step51 frozen artifact changed'
pass 'Step50 lifecycle and Step51 matching/fills remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step52 artifacts are present and hash-clean'
for f in \
 "$CFG/accounting_v1.json" "$CFG/accounting_manifest_v1.json" \
 "$ART/STEP_52_MOCK_ACCOUNT_MARGIN_POSITIONS_ACCOUNTING.md" \
 "$ART/STEP_53_IMPLEMENTATION_HANDOFF.json" "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_accounting_fixed_point_v1.h" \
 "$ROOT/lib/src/exchange/mock_account_margin_positions_accounting_v1.h" \
 "$ROOT/validation/step52_mock_account_margin_positions_accounting_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step52 hash mismatch'
pass 'Step52 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step52 fingerprint binds to exact Step51 matching/fill fingerprint'
python3 -S - "$CFG/matching_fill_manifest_v1.json" "$CFG/accounting_v1.json" "$CFG/accounting_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s51=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step52-v1\n{s51['combinedMatchingFillFingerprint']}\n"
    f"mock-accounting-v1-step52\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep51MatchingFillFingerprint']==s51['combinedMatchingFillFingerprint']
assert m['step51MatchingFillFingerprint']==s51['combinedMatchingFillFingerprint']
assert m['accountingConfigSha256']==sha
assert m['combinedAccountingFingerprint']==fp
assert m['accountProjectionImplemented'] is True
assert m['positionsImplemented'] is True
assert m['realizedUnrealizedPnlImplemented'] is True
assert m['canonicalFeeRebateFundingEventsImplemented'] is True
assert m['marginStateImplemented'] is True
assert m['orderViewsImplemented'] is True
assert m['restartDurabilityImplemented'] is False
assert m['userStreamRecoveryImplemented'] is False
assert m['realVenueReconciliationImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_53_MOCK_SNAPSHOT_USER_STREAM_RECOVERY'
PY
pass 'accounting fingerprint is reproducible and bound to Step51'

printf '%s\n' '[4/8] Economic model is explicit, fill-authoritative and replay-speed neutral'
python3 -S - "$CFG/accounting_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['fillAuthority']=='CANONICAL_FILL_ONLY'
assert d['initialCash']=='100000.00'
assert d['moneyScale']==8
assert d['principalCashMovementOnFill'] is False
assert d['feePosting']['feePpm']==400
assert d['feePosting']['eventType']=='TRADING_FEE'
assert d['rebatePosting']['eventType']=='REBATE'
assert d['fundingPosting']['eventType']=='FUNDING_PAYMENT'
assert d['duplicateFillPolicy']=='IN_PROCESS_IDEMPOTENT_BY_NATIVE_FILL_ID_AND_EXACT_FINGERPRINT'
assert d['conflictingFillIdentityPolicy']=='MARK_ACCOUNTING_UNSAFE_AND_REJECT'
assert d['restartDurability']=='DEFERRED_TO_STEP53'
assert d['wallClockUsedForEconomics'] is False
assert d['openOrdersSource']=='STEP50_CANONICAL_LIFECYCLE_STATE'
assert d['historicalOrdersSource']=='STEP50_CANONICAL_LIFECYCLE_STATE'
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'cash/PnL/equity/margin/fee/funding semantics are explicit and fail-closed'

printf '%s\n' '[5/8] C++17 account/position/PnL/margin/accounting suite passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step52_mock_account_margin_positions_accounting_test.cpp" \
 -o "$TMP/step52_test"
"$TMP/step52_test"
pass 'fill accounting, fees, funding, rebates, positions, PnL, margin and order views validated'

printf '%s\n' '[6/8] Fill remains sole position execution authority and no wall clock enters economics'
grep -Fq 'std::holds_alternative<Fill>' \
 "$ROOT/lib/src/exchange/mock_account_margin_positions_accounting_v1.h" \
 || fail 'canonical Fill consumption path missing'
if grep -Ein 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|gettimeofday|clock_gettime|std::time|time\(' \
 "$ROOT/lib/src/exchange/mock_account_margin_positions_accounting_v1.h" \
 "$ROOT/lib/src/exchange/mock_accounting_fixed_point_v1.h" >"$TMP/clock"; then
 cat "$TMP/clock" >&2
 fail 'wall/monotonic clock primitive leaked into Step52 economics'
fi
pass 'positions change only through canonical Fill and timestamps are supplied event time'

printf '%s\n' '[7/8] Step53/private real-venue concerns remain deferred'
if grep -Ein 'fstream|ofstream|ifstream|sqlite|postgres|nats|jetstream|reconnect|socket|hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|/exchange' \
 "$ROOT/lib/src/exchange/mock_account_margin_positions_accounting_v1.h" \
 "$ROOT/lib/src/exchange/mock_accounting_fixed_point_v1.h" >"$TMP/future"; then
 cat "$TMP/future" >&2
 fail 'Step53 persistence/stream or private real-venue implementation leaked into Step52'
fi
pass 'no restart persistence/user-stream recovery/private real routing is implemented'

printf '%s\n' '[8/8] Step53 handoff is bound to exact Step52 fingerprint'
python3 -S - "$CFG/accounting_manifest_v1.json" "$ART/STEP_53_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step52-to-step53-v1'
assert h['nextStep']=='STEP_53_MOCK_SNAPSHOT_USER_STREAM_RECOVERY'
assert h['step52AccountingFingerprint']==m['combinedAccountingFingerprint']
assert h['step51MatchingFillFingerprint']==m['step51MatchingFillFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert 'durable snapshot of order lifecycle + fill dedup + accounting state' in h['requiredStep53Behavior']
PY
pass 'next safe step is Step53 Mock Snapshot / User Stream / Recovery'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 52: PASS — MOCK ACCOUNT / MARGIN / POSITIONS / ACCOUNTING VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Fill-driven cash/equity/positions/PnL/margin plus canonical fee/rebate/funding accounting are validated.'
printf '%s\n' 'No restart recovery/real reconciliation/private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 53 Mock Snapshot / User Stream / Recovery.'
