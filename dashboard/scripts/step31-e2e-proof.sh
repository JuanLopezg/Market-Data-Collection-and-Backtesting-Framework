#!/usr/bin/env sh
set -eu

BASE_URL="${DASHBOARD_BASE_URL:-http://localhost:8080}"
USERNAME="${DASHBOARD_PROOF_USERNAME:-viewer}"
PASSWORD="${DASHBOARD_PROOF_PASSWORD:-viewer-demo}"
REQUIRE_ALIGNED="${DASHBOARD_PROOF_REQUIRE_ALIGNED:-0}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM
COOKIE_JAR="$TMP_DIR/cookies.txt"
PIPELINE_JSON="$TMP_DIR/pipeline.json"

printf 'Step 31 read-only proof against %s\n' "$BASE_URL"

curl -fsS \
  -c "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\"}" \
  "$BASE_URL/api/auth/login" >/dev/null

curl -fsS \
  -b "$COOKIE_JAR" \
  -H 'Accept: application/json' \
  "$BASE_URL/api/pipeline" >"$PIPELINE_JSON"

python3 - "$PIPELINE_JSON" "$REQUIRE_ALIGNED" <<'PY'
import json, sys
path, require_aligned = sys.argv[1], sys.argv[2] == "1"
with open(path, "r", encoding="utf-8") as f:
    payload = json.load(f)
proof = payload.get("proof")
if not proof:
    print("FAIL: /api/pipeline returned no Step 31 proof block")
    raise SystemExit(2)
print(f"status={proof.get('status')} completion={proof.get('completion')}")
print(f"cycle={proof.get('cycleTimestamp')} correlation={proof.get('correlationId')}")
print(f"orders={proof.get('matchedOrders')}/{proof.get('submitOrders')} fills={proof.get('matchedFills')}/{proof.get('submitOrders')}")
for step in proof.get("steps", []):
    print(f"  {step.get('state', 'UNKNOWN'):8} {step.get('label')}: {step.get('evidence')}")
if require_aligned and proof.get("status") != "ALIGNED":
    print("FAIL: DASHBOARD_PROOF_REQUIRE_ALIGNED=1 but full chain is not ALIGNED")
    raise SystemExit(3)
print("PASS: Step 31 proof endpoint is readable and fail-closed semantics are active")
PY
