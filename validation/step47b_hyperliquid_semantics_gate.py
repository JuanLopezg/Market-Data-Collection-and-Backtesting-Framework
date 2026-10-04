#!/usr/bin/env python3
from pathlib import Path
import csv
import hashlib
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
A = ROOT / "docs/venue/step47a"
B = ROOT / "docs/venue/step47b"

def fail(msg):
    print(f"STEP47B: FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)

def ok(msg):
    print(f"STEP47B: PASS: {msg}")

print("============================================================")
print("STEP 47B — HYPERLIQUID API SURFACE & VENUE SEMANTICS MAPPING")
print("============================================================")

print("[1/7] Step47A canonical multi-exchange baseline is present")
a_json = A / "canonical_venue_architecture.json"
if not a_json.is_file():
    fail("missing Step47A canonical_venue_architecture.json")
a = json.loads(a_json.read_text(encoding="utf-8"))
if a.get("contractVersion") != "step47a-v1":
    fail("unexpected Step47A contractVersion")
if a.get("adapterInterfaceImplemented") is not False:
    fail("Step47A baseline unexpectedly claims adapter implementation")
if a.get("routing", {}).get("silentRealToMockFallback") is not False:
    fail("Step47A fail-closed routing invariant changed")
if a.get("routing", {}).get("silentVenueToVenueFallback") is not False:
    fail("Step47A venue fallback invariant changed")
ok("Step47A baseline remains the architecture authority")

print("[2/7] Step47B frozen artifacts and hashes")
required = [
    "HYPERLIQUID_SOURCE_MANIFEST.md",
    "KNOWN_GAPS_FOR_STEP47C.md",
    "STEP_47B_HYPERLIQUID_SEMANTICS_MAPPING.md",
    "hyperliquid_scope_matrix.csv",
    "hyperliquid_semantics_map.json",
    "SHA256SUMS",
]
for name in required:
    if not (B/name).is_file():
        fail(f"missing {B/name}")
expected = {}
for line in (B/"SHA256SUMS").read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    digest, name = line.split(None, 1)
    expected[name.strip()] = digest
for name, digest in expected.items():
    actual = hashlib.sha256((B/name).read_bytes()).hexdigest()
    if actual != digest:
        fail(f"hash mismatch for {name}")
ok("Step47B artifacts are present and hash-stable")

print("[3/7] Official source manifest is Hyperliquid-only")
manifest = (B/"HYPERLIQUID_SOURCE_MANIFEST.md").read_text(encoding="utf-8")
urls = re.findall(r"https://[^\s)]+", manifest)
if len(urls) < 12:
    fail("source manifest is unexpectedly small")
for url in urls:
    if not url.startswith("https://hyperliquid.gitbook.io/hyperliquid-docs/"):
        fail(f"non-official documentation URL in source manifest: {url}")
ok(f"{len(urls)} official Hyperliquid documentation URLs frozen")

print("[4/7] Structured semantic map covers Step47A capabilities and scope")
m = json.loads((B/"hyperliquid_semantics_map.json").read_text(encoding="utf-8"))
if m.get("contractVersion") != "step47b-v1":
    fail("unexpected Step47B contractVersion")
if m.get("basedOnCanonicalArchitecture") != "step47a-v1":
    fail("Step47B is not bound to Step47A")
if m.get("venueId") != "HYPERLIQUID":
    fail("unexpected venueId")
policy = m.get("documentationPolicy", {})
for key in ("privateCredentialsUsed","signedRequestsSent","ordersSent","capitalMoved","runtimeCodeChanged"):
    if policy.get(key) is not False:
        fail(f"documentation-only invariant violated: {key}")

a_caps = set(a.get("capabilities", []))
assessments = {x["capability"]: x for x in m.get("capabilityAssessment", [])}
if set(assessments) != a_caps:
    missing = sorted(a_caps - set(assessments))
    extra = sorted(set(assessments) - a_caps)
    fail(f"capability assessment mismatch missing={missing} extra={extra}")
if assessments["IDEMPOTENT_SUBMIT"]["support"] != "NOT_CONFIRMED":
    fail("IDEMPOTENT_SUBMIT must remain NOT_CONFIRMED in Step47B")

allowed_map = set(m["mappingClasses"])
allowed_scope = set(m["scopeClasses"])
source_keys = set(re.findall(r"^## ([A-Z0-9_]+) —", manifest, flags=re.M))
if not source_keys:
    fail("could not parse source keys")
semantics = m.get("semantics", [])
if len(semantics) < 50:
    fail("semantic map is unexpectedly incomplete")
ids = set()
gaps = 0
for item in semantics:
    if item["id"] in ids:
        fail(f"duplicate semantic id {item['id']}")
    ids.add(item["id"])
    if item["mappingClass"] not in allowed_map:
        fail(f"invalid mappingClass for {item['id']}")
    if not item["scopeTags"] or any(x not in allowed_scope for x in item["scopeTags"]):
        fail(f"invalid scope tags for {item['id']}")
    if item["sourceKey"] not in source_keys:
        fail(f"unknown sourceKey for {item['id']}: {item['sourceKey']}")
    if item["mappingClass"] == "UNSUPPORTED_BY_CANONICAL_MODEL":
        gaps += 1
if gaps < 6:
    fail("Step47B must surface canonical gaps rather than normalize them away")
ok(f"{len(semantics)} semantics mapped; {gaps} explicit canonical gaps retained for Step47C")

print("[5/7] CSV matrix matches structured semantic IDs")
with (B/"hyperliquid_scope_matrix.csv").open(encoding="utf-8", newline="") as f:
    csv_ids = {row["id"] for row in csv.DictReader(f)}
if csv_ids != ids:
    fail("CSV and JSON semantic IDs diverge")
ok("human CSV and structured JSON mapping are aligned")

print("[6/7] No secret material or private/runtime implementation was introduced")
text = "\n".join((B/name).read_text(encoding="utf-8", errors="replace") for name in required)
# We intentionally discuss private keys conceptually; reject only secret-shaped assignments/material.
secret_patterns = [
    r"(?i)(private[_ -]?key|seed|mnemonic|telegram[_ -]?token)\s*[:=]\s*[\"']?[A-Za-z0-9+/=_-]{24,}",
    r"\b0x[0-9a-fA-F]{64}\b",
]
for pat in secret_patterns:
    if re.search(pat, text):
        fail("secret-like material detected in Step47B artifacts")
if (ROOT/"lib/src/exchange/venue_adapter.h").exists():
    fail("Step47B unexpectedly contains/depends on a Step48 venue_adapter.h implementation")
ok("Step47B remains documentation/mapping only; no credentials or runtime adapter implementation")

print("[7/7] Step47C handoff is explicit")
g = (B/"KNOWN_GAPS_FOR_STEP47C.md").read_text(encoding="utf-8")
needed = [
    "Canonical order lifecycle vocabulary",
    "Idempotent submit",
    "Trigger / TP-SL capability",
    "Fee and funding accounting vocabulary",
    "Canonical product / market identity",
    "Batch operation result envelope",
    "Native fill identity",
]
for phrase in needed:
    if phrase.lower() not in g.lower():
        fail(f"missing Step47C gap: {phrase}")
if m.get("nextStep") != "STEP_47C_PORTABILITY_AND_ARCHITECTURE_FREEZE_GATE":
    fail("nextStep is not Step47C")
ok("known gaps are explicit and next safe step is Step47C")

print()
print("============================================================")
print("STEP 47B: PASS — HYPERLIQUID API SURFACE & VENUE SEMANTICS MAPPING VALIDATED")
print("============================================================")
print("Hyperliquid semantics are mapped against the Step47A multi-exchange baseline.")
print("No private auth/signing/order routing or capital movement was enabled.")
print("Next safe step: Step 47C Portability / Architecture Freeze Gate.")
