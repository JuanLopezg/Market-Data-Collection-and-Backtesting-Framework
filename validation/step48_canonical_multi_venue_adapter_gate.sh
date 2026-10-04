#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ART="$ROOT/docs/venue/step48"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass() { printf 'STEP48: PASS: %s\n' "$*"; }
fail() { printf 'STEP48: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 48 — CANONICAL MULTI-VENUE ADAPTER CONTRACT v1'
printf '%s\n' '============================================================'
printf '%s\n' 'Interface/contracts only. No concrete venue, private auth, signing or routing.'
printf '\n'

printf '%s\n' '[1/8] Step47 frozen evidence remains hash-clean'
for dir in step47a step47b step47c; do
  [[ -f "$ROOT/docs/venue/$dir/SHA256SUMS" ]] || fail "missing $dir SHA256SUMS"
  if [[ "$dir" == "step47a" ]]; then
    (
      cd "$ROOT"
      sha256sum -c "docs/venue/step47a/SHA256SUMS" >/dev/null
    ) || fail "$dir frozen evidence changed"
  else
    (
      cd "$ROOT/docs/venue/$dir"
      sha256sum -c SHA256SUMS >/dev/null
    ) || fail "$dir frozen evidence changed"
  fi
done
pass 'Step47A/B/C frozen artifacts remain intact'

printf '%s\n' '[2/8] Step48 artifact files are present and hash-clean'
for file in \
  "$ART/STEP_48_CANONICAL_MULTI_VENUE_ADAPTER_CONTRACT.md" \
  "$ART/canonical_venue_adapter_contract_v1.json" \
  "$ART/SHA256SUMS" \
  "$ROOT/lib/src/contracts/canonical_venue_identity_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_capabilities_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_errors_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_orders_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_account_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_events_v1.h" \
  "$ROOT/lib/src/exchange/canonical_venue_adapter.h" \
  "$ROOT/validation/step48_canonical_multi_venue_adapter_test.cpp"; do
  [[ -f "$file" ]] || fail "missing ${file#$ROOT/}"
done
(
  cd "$ROOT"
  sha256sum -c "$ART/SHA256SUMS" >/dev/null
) || fail 'Step48 artifact hash mismatch'
pass 'v1 interface/contracts/spec match frozen Step48 hashes'

printf '%s\n' '[3/8] Machine-readable v1 contract preserves fail-closed multi-venue rules'
python3 -S - "$ART/canonical_venue_adapter_contract_v1.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    d=json.load(f)
assert d['contractVersion']=='venue-adapter-v1'
assert d['basedOnFreeze']=='step47c-v1'
assert d['implementationKind']=='INTERFACE_ONLY'
assert d['privateAuthImplemented'] is False
assert d['privateSigningImplemented'] is False
assert d['concreteVenueImplemented'] is False
assert d['routingEnabled'] is False
assert d['venueSelection']=='EXPLICIT_CONFIGURED'
assert d['silentRealToMockFallback'] is False
assert d['silentVenueToVenueFallback'] is False
assert d['smartOrderRouting'] is False
assert d['splitRouting'] is False
assert d['portability']['coreMayBranchOnVenueId'] is False
assert d['portability']['missingRequiredCapability']=='BLOCK_ROUTE_FOR_THAT_VENUE'
assert d['idempotency']['clientOrderIdImpliesNativeIdempotency'] is False
assert d['idempotency']['unknownSubmitOutcome']=='RECONCILE_BEFORE_RESUBMIT'
assert d['nextStep']=='STEP_49_MOCK_VENUE_CATALOG_AND_TRADING_RULES'
PY
pass 'explicit venue selection, no fallback, capability-driven blocking and reconciliation-before-resubmit are frozen'

printf '%s\n' '[4/8] Canonical v1 contract compiles and behavior assertions pass'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$ROOT/lib/src/contracts" \
  -I"$ROOT/lib/src/exchange" \
  -I"$ROOT/lib/src/data_types" \
  "$ROOT/validation/step48_canonical_multi_venue_adapter_test.cpp" \
  -o "$TMP/step48_contract_test"
"$TMP/step48_contract_test"
pass 'CanonicalVenueAdapter and DTO/event vocabulary compile under C++17'

printf '%s\n' '[5/8] v1 canonical headers contain no concrete venue/protocol/auth leakage'
if grep -Ein \
  'hyperliquid|binance|coinbase|kraken|bybit|okx|(^|[^[:alnum:]_])rest([^[:alnum:]_]|$)|websocket|/info|/exchange|private[_ -]?key|seed phrase|mnemonic|signing|nonce|wallet' \
  "$ROOT/lib/src/contracts/canonical_venue_identity_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_capabilities_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_errors_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_orders_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_account_v1.h" \
  "$ROOT/lib/src/contracts/canonical_venue_events_v1.h" \
  "$ROOT/lib/src/exchange/canonical_venue_adapter.h" >"$TMP/leakage"; then
  cat "$TMP/leakage" >&2
  fail 'concrete venue/protocol/private-auth terminology leaked into canonical v1 code'
fi
pass 'canonical v1 code is venue/protocol/auth neutral'

printf '%s\n' '[6/8] Step47C resolutions are implemented in the v1 vocabulary'
python3 -S - "$ART/canonical_venue_adapter_contract_v1.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as f:
    d=json.load(f)
caps=set(d['capabilities'])
assert {'ORDER_STATUS_QUERY','TRIGGER_ORDERS','FEE_ACCOUNTING','FUNDING_ACCOUNTING'} <= caps
assert d['resultEnvelope']['operationPrevalidationFailureFabricatesItemResults'] is False
assert d['accounting']['amount']=='DECIMAL_STRING'
assert 'UNKNOWN_REQUIRES_RECONCILIATION' in d['orderLifecycle']
PY
grep -Fq 'VenueLimit' "$ROOT/lib/src/contracts/canonical_venue_errors_v1.h" || fail 'VENUE_LIMIT missing'
grep -Fq 'UnknownRequiresReconciliation' "$ROOT/lib/src/contracts/canonical_venue_orders_v1.h" || fail 'unknown/reconcile lifecycle missing'
grep -Fq 'FundingPayment' "$ROOT/lib/src/contracts/canonical_venue_account_v1.h" || fail 'funding accounting missing'
pass 'Step47C lifecycle/idempotency/batch/error/accounting/product decisions are represented'

printf '%s\n' '[7/8] Existing gateway/runtime path is not silently rewired'
STEP47A_GATEWAY_HASH="$(awk '$2=="lib/src/exchange/exchange_gateway_adapter.h"{print $1}' "$ROOT/docs/venue/step47a/SHA256SUMS")"
[[ -n "$STEP47A_GATEWAY_HASH" ]] || fail 'could not read Step47A gateway hash'
CURRENT_GATEWAY_HASH="$(sha256sum "$ROOT/lib/src/exchange/exchange_gateway_adapter.h" | awk '{print $1}')"
[[ "$STEP47A_GATEWAY_HASH" == "$CURRENT_GATEWAY_HASH" ]] || fail 'existing ExchangeGatewayAdapter was modified in Step48'
grep -Fq "'canonical_venue_adapter.h'," "$ROOT/lib/src/exchange/meson.build" || fail 'canonical adapter header not registered in exchange source set'
pass 'current runtime gateway behavior remains untouched; new v1 interface is additive only'

printf '%s\n' '[8/8] No concrete adapter/private route exists; Step49 is next'
if find "$ROOT/lib/src" -type f \( -iname '*hyperliquid*adapter*' -o -iname '*mock_exchange_adapter*' \) -print -quit | grep -q .; then
  fail 'concrete Step49+/private adapter implementation detected during Step48'
fi
if grep -Ein 'class[[:space:]]+(MockExchangeAdapter|HyperliquidAdapter)' \
    "$ROOT/lib/src/exchange/canonical_venue_adapter.h" >"$TMP/concrete"; then
  cat "$TMP/concrete" >&2
  fail 'concrete adapter smuggled into canonical interface'
fi
pass 'interface only; no concrete venue, credentials, signing or routing enabled'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 48: PASS — CANONICAL MULTI-VENUE ADAPTER CONTRACT v1 VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Current runtime routing remains unchanged and private/real routing remains disabled.'
printf '%s\n' 'Next safe step: Step 49 Mock Venue Catalog & Trading Rules.'
