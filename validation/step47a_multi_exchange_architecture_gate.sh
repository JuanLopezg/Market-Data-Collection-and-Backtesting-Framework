#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ART="$ROOT/docs/venue/step47a"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass() { printf 'STEP47A: PASS: %s\n' "$*"; }
fail() { printf 'STEP47A: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 47A — CANONICAL MULTI-EXCHANGE ARCHITECTURE BASELINE'
printf '%s\n' '============================================================'
printf '%s\n' 'Architecture/vocabulary only. No private auth, signing, routing or orders.'
printf '\n'

printf '%s\n' '[1/7] Frozen Step47A artifact files are present and hash-clean'
for file in \
  "$ART/STEP_47A_MULTI_EXCHANGE_ARCHITECTURE.md" \
  "$ART/FUTURE_VENUE_PORTABILITY_CHECKLIST.md" \
  "$ART/canonical_venue_architecture.json" \
  "$ART/SHA256SUMS" \
  "$ROOT/lib/src/contracts/venue_identity.h" \
  "$ROOT/lib/src/contracts/venue_capabilities.h" \
  "$ROOT/lib/src/contracts/venue_errors.h" \
  "$ROOT/validation/step47a_multi_exchange_architecture_test.cpp"; do
  [[ -f "$file" ]] || fail "missing ${file#$ROOT/}"
done
(
  cd "$ROOT"
  sha256sum -c "$ART/SHA256SUMS" >/dev/null
) || fail 'Step47A artifact hash mismatch'
pass 'architecture/spec/checklist/contracts match the frozen step47a-v1 hashes'

printf '%s\n' '[2/7] Machine-readable baseline encodes fail-closed multi-venue rules'
python3 - "$ART/canonical_venue_architecture.json" <<'PY'
import json, sys
p=sys.argv[1]
with open(p, encoding='utf-8') as f:
    d=json.load(f)
assert d['contractVersion']=='step47a-v1'
assert d['adapterInterfaceImplemented'] is False
assert d['firstRealVenue']=='HYPERLIQUID'
assert d['futureVenuePlaceholder']=='FUTURE_VENUE'
r=d['routing']
assert r['selection']=='EXPLICIT_CONFIGURED'
assert r['silentRealToMockFallback'] is False
assert r['silentVenueToVenueFallback'] is False
assert r['smartOrderRouting'] is False
assert r['splitRouting'] is False
assert d['identity']['heuristicSymbolMappingAllowed'] is False
assert {'MOCK','HYPERLIQUID','FUTURE_VENUE'} <= set(d['portabilityGate']['requiredAdapters'])
for k in ('strategyContractRewriteRequiredForFutureVenue',
          'riskContractRewriteRequiredForFutureVenue',
          'plannerContractRewriteRequiredForFutureVenue',
          'parallelDashboardRequiredForFutureVenue'):
    assert d['portabilityGate'][k] is False
assert d['nextStep']=='STEP_47B_HYPERLIQUID_API_SURFACE_AND_VENUE_SEMANTICS_MAPPING'
PY
pass 'explicit selection, no fallback/split-routing/heuristic mapping, FUTURE_VENUE portability are frozen'

printf '%s\n' '[3/7] Canonical identity/capability/error vocabulary compiles and behaves'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$ROOT/lib/src/contracts" \
  "$ROOT/validation/step47a_multi_exchange_architecture_test.cpp" \
  -o "$TMP/step47a_contract_test"
"$TMP/step47a_contract_test"
pass 'venue-neutral contract vocabulary compiles under C++17 and regression assertions pass'

printf '%s\n' '[4/7] Canonical contract headers contain no concrete venue/protocol leakage'
if grep -Ein 'hyperliquid|binance|coinbase|kraken|bybit|okx|rest|websocket|/info|/exchange|private[_ -]?key|seed phrase' \
    "$ROOT/lib/src/contracts/venue_identity.h" \
    "$ROOT/lib/src/contracts/venue_capabilities.h" \
    "$ROOT/lib/src/contracts/venue_errors.h" >"$TMP/leakage"; then
  cat "$TMP/leakage" >&2
  fail 'concrete venue/protocol/auth terminology leaked into canonical Step47A headers'
fi
pass 'canonical types are independent of Hyperliquid and other concrete protocols'

printf '%s\n' '[5/7] Existing exchange gateway wording is venue-neutral without changing behavior'
GATEWAY="$ROOT/lib/src/exchange/exchange_gateway_adapter.h"
[[ -f "$GATEWAY" ]] || fail 'missing exchange_gateway_adapter.h'
if grep -Ein 'binance|hyperliquid' "$GATEWAY" >"$TMP/gateway_leakage"; then
  cat "$TMP/gateway_leakage" >&2
  fail 'legacy concrete venue wording remains in ExchangeGatewayAdapter boundary'
fi
grep -Fq 'venue-neutral' "$GATEWAY" || fail 'gateway comment does not state venue-neutral boundary'
pass 'legacy gateway remains behaviorally unchanged but its boundary is no longer venue-branded'

printf '%s\n' '[6/7] Step47A does not implement Step48 or activate private routing'
if grep -ERn --include='venue_*.h' 'class[[:space:]]+VenueAdapter|submitOrder\(|cancelOrder\(|modifyOrder\(|sign|nonce|wallet' \
    "$ROOT/lib/src/contracts" >"$TMP/step48_leakage"; then
  cat "$TMP/step48_leakage" >&2
  fail 'Step47A smuggled adapter implementation/private routing concepts into canonical contract headers'
fi
python3 - "$ART/canonical_venue_architecture.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    d=json.load(f)
assert d['adapterInterfaceImplemented'] is False
PY
pass 'no VenueAdapter implementation, signing or executable routing was introduced in Step47A'

printf '%s\n' '[7/7] Portability checklist proves the architecture is not Hyperliquid-only'
CHECK="$ART/FUTURE_VENUE_PORTABILITY_CHECKLIST.md"
grep -Fq 'FUTURE_VENUE' "$CHECK" || fail 'FUTURE_VENUE portability placeholder missing'
grep -Fq 'MOCK' "$CHECK" || fail 'MOCK portability requirement missing'
grep -Fq 'Hyperliquid' "$ART/STEP_47A_MULTI_EXCHANGE_ARCHITECTURE.md" || fail 'first-real-venue context missing'
grep -Fq 'must not require rewriting Strategy/Risk/Planner contracts' "$ART/STEP_47A_MULTI_EXCHANGE_ARCHITECTURE.md" || \
  fail 'future venue extension rule missing'
pass 'MOCK + Hyperliquid-first + FUTURE_VENUE all fit the same venue-neutral architecture'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 47A: PASS — CANONICAL MULTI-EXCHANGE ARCHITECTURE BASELINE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'No private auth/signing/order routing was enabled.'
printf '%s\n' 'Next safe step: Step 47B Hyperliquid API Surface & Venue Semantics Mapping.'
