#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step51"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP51: PASS: %s\n' "$*"; }
fail(){ printf 'STEP51: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 51 — DETERMINISTIC MATCHING / FILL MODEL'
printf '%s\n' '============================================================'
printf '%s\n' 'Synthetic OHLCV model. No account/PnL/restart/private real-venue routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step49 + Step50 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step49/SHA256SUMS >/dev/null) || fail 'Step49 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step50/SHA256SUMS >/dev/null) || fail 'Step50 frozen artifact changed'
pass 'Step49 catalog/rules and Step50 lifecycle remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step51 artifacts are present and hash-clean'
for f in \
 "$CFG/matching_fill_model_v1.json" "$CFG/matching_fill_manifest_v1.json" \
 "$ART/STEP_51_DETERMINISTIC_MATCHING_FILL_MODEL.md" \
 "$ART/STEP_52_IMPLEMENTATION_HANDOFF.json" "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_deterministic_matching_fill_v1.h" \
 "$ROOT/validation/step51_deterministic_matching_fill_test.cpp"; do
 [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step51 hash mismatch'
pass 'Step51 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step51 fingerprint binds to exact Step50 lifecycle fingerprint'
python3 -S - "$CFG/order_lifecycle_manifest_v1.json" "$CFG/matching_fill_model_v1.json" "$CFG/matching_fill_manifest_v1.json" <<'PY2'
import hashlib,json,pathlib,sys
s50=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
model=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
 f"step51-v1\n{s50['combinedOrderLifecycleFingerprint']}\n"
 f"mock-matching-fill-v1-step51\n{sha}\n".encode()).hexdigest()
assert model['boundStep50OrderLifecycleFingerprint']==s50['combinedOrderLifecycleFingerprint']
assert m['step50OrderLifecycleFingerprint']==s50['combinedOrderLifecycleFingerprint']
assert m['modelConfigSha256']==sha
assert m['combinedMatchingFillFingerprint']==fp
assert m['matcherImplemented'] is True
assert m['canonicalFillEventsImplemented'] is True
assert m['accountingPostingImplemented'] is False
assert m['positionsImplemented'] is False
assert m['restartDurabilityImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_52_MOCK_ACCOUNT_MARGIN_POSITIONS_ACCOUNTING'
PY2
pass 'matching/fill fingerprint is deterministic and bound to Step50'

printf '%s\n' '[4/8] Model is explicitly synthetic, deterministic and replay-speed neutral'
python3 -S - "$CFG/matching_fill_model_v1.json" <<'PY2'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['marketInput']=='OHLCV_BAR_SYNTHETIC'
assert d['historicalL2Claim'] is False
assert d['liquidityModel']['sharedAcrossOrdersForSameBar'] is True
assert d['liquidityModel']['allocationOrder']=='ASCENDING_LOCAL_ORDER_ID'
assert d['slippageModel']['deterministicSeed']==510051
assert d['slippageModel']['neverWorseThanLimit'] is True
assert d['feeModel']['computedAsDiagnosticQuoteOnly'] is True
assert d['feeModel']['accountingEventPosting']=='DEFERRED_TO_STEP52'
assert d['latencyModel']['unit']=='MARKET_EVENTS_FOR_SAME_ASSET'
assert d['latencyModel']['wallClockUsedForEconomics'] is False
assert d['timeSemantics']['fillTimestamp']=='MARKET_OBSERVATION_EVENT_TIME'
assert d['timeSemantics']['wallClockOrMonotonicEconomicTimestamp'] is False
assert d['accountingImplemented'] is False
assert d['restartDurabilityImplemented'] is False
PY2
pass 'OHLCV synthetic assumptions, shared liquidity, seed, latency and event-time semantics are explicit'

printf '%s\n' '[5/8] C++17 deterministic matcher/fill lifecycle suite passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step51_deterministic_matching_fill_test.cpp" \
 -o "$TMP/step51_test"
"$TMP/step51_test"
pass 'GTC/IOC/post-only/partial/full/shared-liquidity/latency/determinism behavior validated'

printf '%s\n' '[6/8] Step51 uses event time and contains no wall-clock economics'
if grep -Ein 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|time\(|gettimeofday|clock_gettime' \
 "$ROOT/lib/src/exchange/mock_deterministic_matching_fill_v1.h" >"$TMP/clock"; then
 cat "$TMP/clock" >&2
 fail 'wall/monotonic clock primitive leaked into Step51 economic model'
fi
grep -Fq 'observation.event_time' "$ROOT/lib/src/exchange/mock_deterministic_matching_fill_v1.h" \
 || fail 'event-time fill timestamp not found'
pass 'economic matching/fill timestamps depend only on supplied market event time'

printf '%s\n' '[7/8] No Step52 accounting or private real-venue implementation slipped in'
if grep -Ein 'cash_balance|equity_state|realized_pnl|unrealized_pnl|position_ledger|post_accounting_event|apply_funding' \
 "$ROOT/lib/src/exchange/mock_deterministic_matching_fill_v1.h" >"$TMP/account"; then
 cat "$TMP/account" >&2
 fail 'Step52 account/PnL implementation leaked into Step51'
fi
if grep -Ein 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|/exchange' \
 "$ROOT/lib/src/exchange/mock_deterministic_matching_fill_v1.h" >"$TMP/private"; then
 cat "$TMP/private" >&2
 fail 'private/concrete real-venue material leaked into Step51'
fi
pass 'canonical Fill exists, but account/PnL/private real routing remain deferred'

printf '%s\n' '[8/8] Step52 handoff is bound to exact Step51 fingerprint'
python3 -S - "$CFG/matching_fill_manifest_v1.json" "$ART/STEP_52_IMPLEMENTATION_HANDOFF.json" <<'PY2'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step51-to-step52-v1'
assert h['nextStep']=='STEP_52_MOCK_ACCOUNT_MARGIN_POSITIONS_ACCOUNTING'
assert h['step51MatchingFillFingerprint']==m['combinedMatchingFillFingerprint']
assert h['step50OrderLifecycleFingerprint']==m['step50OrderLifecycleFingerprint']
assert h['step51FeeModel']['step51Behavior']=='DIAGNOSTIC_QUOTE_ONLY'
assert h['step51FeeModel']['step52MustPostCanonicalAccounting'] is True
assert 'private Hyperliquid signing/routing' in h['forbiddenUntilLater']
PY2
pass 'next safe step is Step52 Mock Account / Margin / Positions / Accounting'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 51: PASS — DETERMINISTIC MATCHING / FILL MODEL VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Synthetic OHLCV matching, shared liquidity, deterministic fills and lifecycle transitions are validated.'
printf '%s\n' 'No account/PnL/restart/private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 52 Mock Account / Margin / Positions / Accounting.'
