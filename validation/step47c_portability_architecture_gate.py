#!/usr/bin/env python3
from pathlib import Path
import csv, hashlib, json, subprocess, sys

ROOT = Path(__file__).resolve().parents[1]
A = ROOT / "docs/venue/step47a"
B = ROOT / "docs/venue/step47b"
C = ROOT / "docs/venue/step47c"

def fail(msg):
    print(f"STEP47C: FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)

def ok(msg):
    print(f"STEP47C: PASS: {msg}")

print("============================================================")
print("STEP 47C — PORTABILITY / ARCHITECTURE FREEZE GATE")
print("============================================================")

print("[1/8] Step47A and Step47B preconditions remain valid")
for script in (
    ROOT / "validation/step47a_multi_exchange_architecture_gate.sh",
    ROOT / "validation/step47b_hyperliquid_semantics_gate.sh",
):
    if not script.is_file():
        fail(f"missing prerequisite gate {script.relative_to(ROOT)}")
    proc = subprocess.run(["bash", str(script)], cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print(proc.stdout)
        fail(f"prerequisite gate failed: {script.name}")
ok("Step47A architecture baseline and Step47B Hyperliquid mapping still pass")

print("[2/8] Step47C artifacts are present and hash-stable")
required = [
    "FUTURE_VENUE_CONFORMANCE.json",
    "GAP_RESOLUTION_MATRIX.csv",
    "HYPERLIQUID_V1_SCOPE_FREEZE.md",
    "STEP_47C_PORTABILITY_ARCHITECTURE_FREEZE.md",
    "STEP_48_IMPLEMENTATION_HANDOFF.json",
    "canonical_multi_venue_contract_freeze.json",
    "SHA256SUMS",
]
for name in required:
    if not (C/name).is_file():
        fail(f"missing docs/venue/step47c/{name}")
expected = {}
for line in (C/"SHA256SUMS").read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    digest, name = line.split(None, 1)
    expected[name.strip()] = digest
for name, digest in expected.items():
    actual = hashlib.sha256((C/name).read_bytes()).hexdigest()
    if actual != digest:
        fail(f"hash mismatch for {name}")
ok("Step47C freeze artifacts match SHA256SUMS")

print("[3/8] All Step47B required gaps are explicitly resolved")
b = json.loads((B/"hyperliquid_semantics_map.json").read_text(encoding="utf-8"))
required_gaps = set(b["step47cRequiredGaps"])
gap_id_map = {
    "GAP-001":"CANONICAL_ORDER_LIFECYCLE_VOCABULARY",
    "GAP-002":"IDEMPOTENT_SUBMIT_POLICY",
    "GAP-003":"TRIGGER_TPSL_CAPABILITY_DECISION",
    "GAP-004":"FEE_AND_FUNDING_ACCOUNTING_VOCABULARY",
    "GAP-005":"CANONICAL_PRODUCT_MARKET_IDENTITY",
    "GAP-006":"BATCH_OPERATION_RESULT_ENVELOPE",
    "GAP-007":"VENUE_RISK_LIMIT_REJECT_TAXONOMY",
    "GAP-008":"NATIVE_FILL_IDENTITY_RULE",
    "GAP-009":"V1_HYPERLIQUID_PRODUCT_SCOPE_FREEZE",
}
with (C/"GAP_RESOLUTION_MATRIX.csv").open(encoding="utf-8", newline="") as f:
    rows = list(csv.DictReader(f))
resolved = set()
for row in rows:
    if row["resolution_status"] not in {"RESOLVED","RESOLVED_DEFERRED"}:
        fail(f"unresolved gap {row['gap_id']}")
    if row["gap_id"] not in gap_id_map:
        fail(f"unexpected gap id {row['gap_id']}")
    resolved.add(gap_id_map[row["gap_id"]])
if resolved != required_gaps:
    fail(f"gap coverage mismatch missing={sorted(required_gaps-resolved)} extra={sorted(resolved-required_gaps)}")
ok(f"all {len(required_gaps)} Step47B architecture gaps are explicitly resolved")

print("[4/8] Final canonical freeze encodes fail-closed portable semantics")
c = json.loads((C/"canonical_multi_venue_contract_freeze.json").read_text(encoding="utf-8"))
if c["contractVersion"] != "step47c-v1":
    fail("unexpected contractVersion")
if c["basedOn"] != {"step47a":"step47a-v1","step47b":"step47b-v1"}:
    fail("freeze is not bound to Step47A + Step47B")
if c["adapterInterfaceImplemented"] is not False or c["privateRoutingEnabled"] is not False:
    fail("Step47C must remain architecture-only")
arch = c["architecture"]
for k in ("silentRealToMockFallback","silentVenueToVenueFallback","smartOrderRouting","splitRouting",
          "coreMayBranchOnVenueId","dashboardRequiresParallelVenueSpecificUI"):
    if arch[k] is not False:
        fail(f"portable fail-closed invariant violated: {k}")
if c["idempotencyPolicyV1"]["clientOrderIdImpliesNativeIdempotency"] is not False:
    fail("client order id must not imply native idempotency")
if c["idempotencyPolicyV1"]["hyperliquidNativeIdempotentSubmit"] != "NOT_CONFIRMED":
    fail("Hyperliquid idempotent submit must remain NOT_CONFIRMED")
if c["orderLifecycleV1"]["blindRetryFromUnknownAllowed"] is not False:
    fail("unknown submit outcome must not allow blind retry")
if "VENUE_LIMIT" not in c["canonicalRejectTaxonomyV1"]:
    fail("VENUE_LIMIT resolution missing")
for cap in ("ORDER_STATUS_QUERY","TRIGGER_ORDERS","FEE_ACCOUNTING","FUNDING_ACCOUNTING"):
    if cap not in c["canonicalCapabilitiesV1"]:
        fail(f"resolved capability missing: {cap}")
ok("lifecycle, idempotency, accounting, batch, reject and product semantics are frozen")

print("[5/8] Hyperliquid v1 scope is narrow and does not expand capital/private scope")
s = c["hyperliquidV1Scope"]
if s["productScope"]["DEFAULT_PERPETUALS"] != "IN_SCOPE":
    fail("default perpetuals must be the first Hyperliquid v1 product scope")
if s["productScope"]["SPOT"] != "DEFERRED" or s["productScope"]["HIP3_PERPETUALS"] != "DEFERRED":
    fail("spot/HIP3 must remain deferred")
if s["productScope"]["OUTCOMES"] != "OUT_OF_SCOPE":
    fail("outcomes must remain out of scope")
if s["featureScope"]["CAPITAL_TRANSFERS_WITHDRAWALS_STAKING_VAULT_ACTIONS"] != "OUT_OF_SCOPE":
    fail("capital movement accidentally entered adapter v1")
if "DEFERRED" not in s["privateAuthSigning"]:
    fail("private auth/signing was not kept deferred")
ok("Hyperliquid v1 is default-perpetuals-first; private auth and capital movement remain deferred/out of scope")

print("[6/8] FUTURE_VENUE proves capability-driven portability without core rewrite")
p = json.loads((C/"FUTURE_VENUE_CONFORMANCE.json").read_text(encoding="utf-8"))
profiles = {x["venueId"]: x for x in p["profiles"]}
if set(profiles) != {"MOCK","HYPERLIQUID","FUTURE_VENUE"}:
    fail("portability profiles are incomplete")
fv = profiles["FUTURE_VENUE"]
for k in ("requiresCoreRewrite","requiresStrategyRewrite","requiresRiskRewrite",
          "requiresPlannerRewrite","requiresParallelDashboard"):
    if fv[k] is not False:
        fail(f"FUTURE_VENUE portability failed: {k}")
if fv["unsupportedRequiredCapabilityAction"] != "BLOCK_ROUTE_FOR_THAT_VENUE":
    fail("missing fail-closed capability behavior")
if set(fv["supportedCapabilities"]) & set(fv["unsupportedCapabilities"]):
    fail("future venue supported/unsupported capability sets overlap")
ok("FUTURE_VENUE can differ materially without Strategy/Risk/Planner/dashboard rewrites")

print("[7/8] No Step48 implementation or private routing code is smuggled into Step47C")
# Step47C delta is docs + validation only by design.
for pth in C.rglob("*"):
    if pth.suffix in {".h",".hpp",".cpp",".cc",".cxx",".go",".rs"}:
        fail(f"runtime/code artifact present in Step47C docs: {pth.relative_to(ROOT)}")
handoff = json.loads((C/"STEP_48_IMPLEMENTATION_HANDOFF.json").read_text(encoding="utf-8"))
if handoff["mustNotEnablePrivateRouting"] is not True:
    fail("Step48 handoff does not preserve private-routing boundary")
if "Hyperliquid signing implementation" not in handoff["forbiddenStep48Features"]:
    fail("Step48 private signing prohibition missing")
ok("Step47C is a pure architecture freeze; implementation remains Step48")

print("[8/8] Next safe step is Step48")
if c["nextStep"] != "STEP_48_CANONICAL_MULTI_VENUE_ADAPTER_CONTRACT_V1":
    fail("canonical freeze nextStep is not Step48")
if handoff["nextStep"] != c["nextStep"]:
    fail("Step48 handoff and canonical freeze disagree")
ok("Step47 umbrella can close with Step48 as the next safe step")

print()
print("============================================================")
print("STEP 47C: PASS — MULTI-VENUE PORTABILITY / ARCHITECTURE FREEZE VALIDATED")
print("============================================================")
print("Step47A + Step47B inputs remain valid and all recorded gaps are resolved.")
print("No private auth/signing/order routing or capital movement was enabled.")
print("Next safe step: Step 48 Canonical Multi-Venue Adapter Contract v1.")
