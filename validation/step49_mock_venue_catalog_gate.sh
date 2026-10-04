#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step49"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP49: PASS: %s\n' "$*"; }
fail(){ printf 'STEP49: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 49 — MOCK VENUE CATALOG & TRADING RULES'
printf '%s\n' '============================================================'
printf '%s\n' 'Catalog/rules only. No order admission, matching, accounting or real/private routing.'
printf '\n'

printf '%s\n' '[1/8] Step48 canonical multi-venue contract still passes'
bash "$ROOT/validation/step48_canonical_multi_venue_adapter_gate.sh" "$ROOT" >"$TMP/step48.log" 2>&1 || {
  cat "$TMP/step48.log" >&2
  fail 'Step48 prerequisite failed'
}
grep -Fq 'STEP 48: PASS — CANONICAL MULTI-VENUE ADAPTER CONTRACT v1 VALIDATED' "$TMP/step48.log" || fail 'Step48 PASS banner missing'
pass 'Step48 remains intact'

printf '%s\n' '[2/8] Step49 artifacts are present and hash-clean'
for f in \
 "$CFG/catalog_v1.json" "$CFG/rules_v1.json" "$CFG/manifest_v1.json" \
 "$CFG/source_registry_assets_step37a5.txt" \
 "$ART/STEP_49_MOCK_VENUE_CATALOG_AND_TRADING_RULES.md" \
 "$ART/STEP_50_IMPLEMENTATION_HANDOFF.json" "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_venue_catalog_v1.h" \
 "$ROOT/lib/src/exchange/mock_venue_catalog_data_v1.inc" \
 "$ROOT/lib/src/exchange/mock_venue_catalog_version_v1.h" \
 "$ROOT/lib/src/exchange/mock_venue_rules_v1.h" \
 "$ROOT/validation/step49_mock_venue_catalog_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step49 hash mismatch'
pass 'Step49 artifacts match frozen hashes'

printf '%s\n' '[3/8] Manifest fingerprints recompute exactly'
python3 -S - "$CFG" <<'PY'
import hashlib,json,pathlib,sys
cfg=pathlib.Path(sys.argv[1])
cat=(cfg/'catalog_v1.json').read_bytes()
rules=(cfg/'rules_v1.json').read_bytes()
src=(cfg/'source_registry_assets_step37a5.txt').read_bytes()
m=json.loads((cfg/'manifest_v1.json').read_text())
cs=hashlib.sha256(cat).hexdigest()
rs=hashlib.sha256(rules).hexdigest()
ss=hashlib.sha256(src).hexdigest()
fp=hashlib.sha256(
    f"step49-v1\nmock-catalog-v1-step49\n{cs}\nmock-rules-v1-step49\n{rs}\n{ss}\n".encode()
).hexdigest()
assert m['catalogSha256']==cs
assert m['rulesSha256']==rs
assert m['sourceUniverseSha256']==ss
assert m['combinedVenueFingerprint']==fp
assert m['entryCount']==175
assert m['realVenueParityClaim'] is False
assert m['adapterImplemented'] is False
assert m['orderAdmissionImplemented'] is False
assert m['matchingImplemented'] is False
assert m['accountingImplemented'] is False
assert m['privateRoutingEnabled'] is False
assert m['nextStep']=='STEP_50_MOCK_ORDER_ADMISSION_AND_LIFECYCLE'
PY
pass 'catalog/rules/source-universe fingerprint is deterministic and versioned'

printf '%s\n' '[4/8] Explicit MOCK catalog covers the frozen 175-asset universe'
python3 -S - "$CFG/catalog_v1.json" "$CFG/source_registry_assets_step37a5.txt" <<'PY'
import json,pathlib,sys
c=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))
source=[x for x in pathlib.Path(sys.argv[2]).read_text(encoding='utf-8').splitlines() if x]
e=c['entries']
assert c['mappingPolicy']=='EXPLICIT_ONLY'
assert c['venueId']=='MOCK' and c['environment']=='MOCK'
assert c['entryCount']==len(e)==len(source)==175
by={x['canonicalAsset']:x for x in e}
assert len(by)==175 and set(by)==set(source)
symbols=set(); native=set()
for a in source:
    x=by[a]
    assert x['routingStatus']=='ENABLED'
    assert x['productClass']=='PERPETUAL'
    assert x['settlementAsset']=='USD'
    assert x['venueSymbol']==a
    assert x['ruleProfileId']=='MOCK_PERP_DEFAULT_V1'
    assert x['venueSymbol'] not in symbols
    assert x['venueAssetId'] not in native
    symbols.add(x['venueSymbol']); native.add(x['venueAssetId'])
for a in ['BTCUSDT','2ZUSDT','MARSCOINUSDT','SOONUSDT','USUSDT','龙虾USDT']:
    assert a in by
PY
pass 'all 175 assets have explicit unique MOCK mappings'

printf '%s\n' '[5/8] Synthetic trading rules are exact-base10 and complete'
python3 -S - "$CFG/rules_v1.json" <<'PY'
import json,pathlib,re,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['policy']=='SYNTHETIC_EXPLICIT_V1'
assert len(d['profiles'])==1
p=d['profiles'][0]
assert p['profileId']=='MOCK_PERP_DEFAULT_V1'
assert p['productClass']=='PERPETUAL'
assert p['priceIncrement']=='0.00000001' and p['priceScale']==8
assert p['sizeIncrement']=='0.00000001' and p['sizeScale']==8
assert p['minSize']=='0.00000001'
assert p['minNotional']=='10.00'
assert p['maxLeverage']==20
assert set(p['marginModes'])=={'CROSS','ISOLATED'}
assert set(p['supportedTimeInForce'])=={'GTC','IOC'}
assert p['postOnly'] and p['reduceOnly'] and p['modify']
assert p['clientOrderId'] and p['nativeIdempotentSubmit']
assert p['admissionPrecisionPolicy']=='REJECT_NONCONFORMING'
assert p['enabled']
PY
pass 'tick/lot/precision/minimum/leverage/margin/TIF metadata is frozen'

printf '%s\n' '[6/8] C++ catalog/rules compile and exact lookup behavior passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step49_mock_venue_catalog_test.cpp" -o "$TMP/step49_test"
"$TMP/step49_test"
pass '175-entry catalog and rules compile under C++17'

printf '%s\n' '[7/8] No heuristic mapping or Step50+/private implementation slipped in'
if grep -Ein 'tolower|toupper|regex|substr|erase|ends_with|starts_with|fuzzy' \
 "$ROOT/lib/src/exchange/mock_venue_catalog_v1.h" \
 "$ROOT/lib/src/exchange/mock_venue_catalog_data_v1.inc" >"$TMP/h"; then
  cat "$TMP/h" >&2
  fail 'heuristic mapping primitive detected'
fi
if find "$ROOT/lib/src" -type f \( -iname '*mock_exchange_adapter*' -o -iname '*matching_engine*' \) -print -quit | grep -q .; then
  fail 'Step50/51 implementation detected during Step49'
fi
pass 'exact-only catalog; no adapter/matcher/private route exists yet'

printf '%s\n' '[8/8] Step50 handoff is bound to the exact Step49 fingerprint'
python3 -S - "$CFG/manifest_v1.json" "$ART/STEP_50_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step49-to-step50-v1'
assert h['nextStep']=='STEP_50_MOCK_ORDER_ADMISSION_AND_LIFECYCLE'
assert h['venueFingerprint']==m['combinedVenueFingerprint']
assert h['entryCount']==m['entryCount']==175
assert 'matching engine/fill-price model' in h['forbiddenUntilLater']
assert 'private real-venue auth/signing/routing' in h['forbiddenUntilLater']
PY
pass 'next safe step is Step50 Mock Order Admission & Lifecycle'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 49: PASS — MOCK VENUE CATALOG & TRADING RULES VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'MOCK has an explicit 175-asset catalog, deterministic synthetic rules and a frozen venue fingerprint.'
printf '%s\n' 'No order admission/matching/accounting/private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 50 Mock Order Admission & Lifecycle.'
