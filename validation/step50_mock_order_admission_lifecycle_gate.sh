#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step50"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP50: PASS: %s\n' "$*"; }
fail(){ printf 'STEP50: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 50 — MOCK ORDER ADMISSION & LIFECYCLE'
printf '%s\n' '============================================================'
printf '%s\n' 'No matching/fills/accounting/private real-venue routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step48 + Step49 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step48/SHA256SUMS >/dev/null) || fail 'Step48 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step49/SHA256SUMS >/dev/null) || fail 'Step49 frozen artifact changed'
pass 'Step48 contract and Step49 catalog/rules remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step50 artifacts are present and hash-clean'
for f in \
 "$CFG/order_lifecycle_v1.json" "$CFG/order_lifecycle_manifest_v1.json" \
 "$ART/STEP_50_MOCK_ORDER_ADMISSION_AND_LIFECYCLE.md" \
 "$ART/STEP_51_IMPLEMENTATION_HANDOFF.json" "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock/decimal.h" \
 "$ROOT/lib/src/exchange/mock/orders.h" \
 "$ROOT/validation/step50_mock_order_admission_lifecycle_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step50 hash mismatch'
pass 'Step50 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step50 fingerprint binds to the exact Step49 fingerprint'
python3 -S - "$CFG/manifest_v1.json" "$CFG/order_lifecycle_v1.json" "$CFG/order_lifecycle_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s49=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
life=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step50-v1\n{s49['combinedVenueFingerprint']}\n"
    f"mock-order-lifecycle-v1-step50\n{sha}\n".encode()
).hexdigest()
assert life['boundStep49VenueFingerprint']==s49['combinedVenueFingerprint']
assert m['step49VenueFingerprint']==s49['combinedVenueFingerprint']
assert m['lifecycleConfigSha256']==sha
assert m['combinedOrderLifecycleFingerprint']==fp
assert m['engineImplemented'] is True
assert m['canonicalVenueAdapterImplemented'] is False
assert m['matchingImplemented'] is False
assert m['fillsImplemented'] is False
assert m['accountingImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_51_DETERMINISTIC_MATCHING_AND_FILL_MODEL'
PY
pass 'Step50 lifecycle fingerprint is reproducible and bound to Step49'

printf '%s\n' '[4/8] Admission/lifecycle policy is fail-closed and correctly staged'
python3 -S - "$CFG/order_lifecycle_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['requestIdPolicy']['sameIdSamePayload']=='RETURN_CACHED_OPERATION_RESULT_NO_DUPLICATE_STATE_MUTATION'
assert d['requestIdPolicy']['sameIdDifferentPayload']=='OPERATION_LEVEL_DUPLICATE_REQUEST'
assert d['requestIdPolicy']['restartDurability']=='DEFERRED_TO_STEP53'
assert d['clientOrderIdPolicy']['reuseAfterCancel'] is False
assert d['admission']['catalogLookup']=='EXACT_ONLY'
assert d['admission']['pricePrecision']=='REJECT_NONCONFORMING'
assert d['admission']['sizePrecision']=='REJECT_NONCONFORMING'
assert d['admission']['minimumNotional']=='ENFORCE_EXACT_FIXED_POINT'
assert d['admission']['postOnlyCrossingDecision']=='DEFER_TO_STEP51_MATCHER'
assert d['admission']['iocRemainderCancellation']=='DEFER_TO_STEP51_MATCHER'
assert d['admission']['reduceOnlyEconomicCheck']=='DEFER_TO_STEP52_ACCOUNT_STATE'
assert d['submitInitialStatus']=='ACCEPTED'
assert d['cancelTransition']==['CANCEL_PENDING','CANCELED']
assert d['matchingImplemented'] is False
assert d['fillsImplemented'] is False
assert d['accountingImplemented'] is False
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'idempotency, exact admission and deferred economic checks are explicit'

printf '%s\n' '[5/8] C++17 admission + submit/cancel/modify lifecycle suite passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step50_mock_order_admission_lifecycle_test.cpp" \
 -o "$TMP/step50_test"
"$TMP/step50_test"
pass 'order admission, batch results, idempotency, cancel/modify and transition safety validated'

printf '%s\n' '[6/8] No Step51/52 economic implementation is present'
if grep -Ein \
 'matching[_ -]?engine|slippage|commission|funding_payment|realized_pnl|unrealized_pnl|cash_balance|apply_fill|create_fill' \
 "$ROOT/lib/src/exchange/mock/orders.h" \
 "$ROOT/lib/src/exchange/mock/decimal.h" >"$TMP/leak"; then
  cat "$TMP/leak" >&2
  fail 'Step51/52 implementation leaked into Step50'
fi
if find "$ROOT/lib/src/exchange" -maxdepth 1 -type f \
   \( -iname '*matching*' -o -iname '*fill_model*' -o -iname '*mock_exchange_adapter*' \) \
   -print -quit | grep -q .; then
  fail 'Step51+ matcher/fill/full-adapter file detected'
fi
pass 'Step50 remains admission/lifecycle only'

printf '%s\n' '[7/8] Private real-venue boundary remains untouched'
if grep -Ein 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|sign[_ -]?transaction|api wallet|/exchange' \
 "$ROOT/lib/src/exchange/mock/orders.h" \
 "$ROOT/lib/src/exchange/mock/decimal.h" >"$TMP/private"; then
  cat "$TMP/private" >&2
  fail 'private/concrete real-venue material leaked into Step50 code'
fi
pass 'no private auth/signing/real routing entered the MOCK lifecycle engine'

printf '%s\n' '[8/8] Step51 handoff is bound to the exact Step50 fingerprint'
python3 -S - "$CFG/order_lifecycle_manifest_v1.json" "$ART/STEP_51_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step50-to-step51-v1'
assert h['nextStep']=='STEP_51_DETERMINISTIC_MATCHING_AND_FILL_MODEL'
assert h['step50OrderLifecycleFingerprint']==m['combinedOrderLifecycleFingerprint']
assert h['step49VenueFingerprint']==m['step49VenueFingerprint']
assert h['mustUseStep50LifecycleBridge'] is True
assert 'cash/equity/position accounting' in h['forbiddenUntilLater']
assert 'private Hyperliquid signing/routing' in h['forbiddenUntilLater']
PY
pass 'next safe step is Step51 Deterministic Matching / Fill Model'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 50: PASS — MOCK ORDER ADMISSION & LIFECYCLE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Submit/cancel/modify, exact admission, idempotency and canonical lifecycle are validated.'
printf '%s\n' 'No matching/fills/accounting/private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 51 Deterministic Matching / Fill Model.'
