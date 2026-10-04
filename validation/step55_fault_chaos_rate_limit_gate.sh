#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step55"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP55: PASS: %s\n' "$*"; }
fail(){ printf 'STEP55: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 55 — FAULT / CHAOS / RATE-LIMIT ENGINE'
printf '%s\n' '============================================================'
printf '%s\n' 'Deterministic MOCK failures only. No full-system/private real-venue routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step53 + Step54 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step53/SHA256SUMS >/dev/null) || fail 'Step53 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step54/SHA256SUMS >/dev/null) || fail 'Step54 frozen artifact changed'
pass 'Step53 recovery and Step54 reconciliation/ledger remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step55 artifacts are present and hash-clean'
for f in \
 "$CFG/fault_chaos_rate_limit_v1.json" \
 "$CFG/fault_chaos_rate_limit_manifest_v1.json" \
 "$ART/STEP_55_FAULT_CHAOS_RATE_LIMIT_ENGINE.md" \
 "$ART/STEP_56_IMPLEMENTATION_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_chaos_prng_v1.h" \
 "$ROOT/lib/src/exchange/mock_fault_chaos_rate_limit_v1.h" \
 "$ROOT/validation/step55_fault_chaos_rate_limit_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step55 hash mismatch'
pass 'Step55 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step55 fingerprint binds to exact Step54 reconciliation/ledger fingerprint'
python3 -S - "$CFG/reconciliation_ledger_manifest_v1.json" "$CFG/fault_chaos_rate_limit_v1.json" "$CFG/fault_chaos_rate_limit_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s54=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step55-v1\n{s54['combinedReconciliationLedgerFingerprint']}\n"
    f"mock-fault-chaos-rate-limit-v1-step55\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep54ReconciliationLedgerFingerprint']==s54['combinedReconciliationLedgerFingerprint']
assert m['step54ReconciliationLedgerFingerprint']==s54['combinedReconciliationLedgerFingerprint']
assert m['faultConfigSha256']==sha
assert m['combinedFaultChaosFingerprint']==fp
assert m['disconnectReconnectImplemented'] is True
assert m['staleSnapshotImplemented'] is True
assert m['delayedResponseImplemented'] is True
assert m['lostAmbiguousSubmitImplemented'] is True
assert m['duplicateOutOfOrderInjectionImplemented'] is True
assert m['canonicalRejectInjectionImplemented'] is True
assert m['deterministicRateLimitImplemented'] is True
assert m['venueUnavailableImplemented'] is True
assert m['deterministicSeedEvidenceImplemented'] is True
assert m['reconciliationSafetyGatePreserved'] is True
assert m['fullSystemStrategyRiskPlannerWiringImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_56_FULL_SYSTEM_REPLAY_RUNTIME'
PY
pass 'fault/chaos fingerprint is reproducible and bound to Step54'

printf '%s\n' '[4/8] Fault, ambiguity, rate-limit and routing-safety policies are explicit'
python3 -S - "$CFG/fault_chaos_rate_limit_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['seed']==550055
assert d['deterministicSelector']=='SPLITMIX64_SEED_PLUS_OPERATION_ORDINAL'
assert d['lostSubmitResponse']['underlyingSubmitMayHaveBeenApplied'] is True
assert d['lostSubmitResponse']['blindRetryAllowed'] is False
assert d['lostSubmitResponse']['requiredNextAction']=='RECONCILE_BEFORE_RETRY'
assert d['delayedResponse']['usesWallClockTimer'] is False
assert d['connectivity']['userStreamDisconnectedBlocksNewSubmit'] is True
assert d['connectivity']['cancelModifyBlockedByNewOrderReconciliationGate'] is False
assert d['connectivity']['reconnectRequiresFreshReconciliationBeforeNewSubmit'] is True
assert d['rateLimit']['model']=='FIXED_EVENT_TIME_WINDOW'
assert d['rateLimit']['eventTimeWindowUnits']==10
assert d['rateLimit']['rateLimitedError']=='RATE_LIMITED'
assert d['rateLimit']['retryable'] is True
assert d['reconciliationSafety']['staleSnapshot']=='PENDING'
assert d['reconciliationSafety']['ambiguousSubmit']=='BLOCK_NEW_SUBMIT_UNTIL_RECONCILED'
assert d['evidence']['hash']=='SHA256'
assert d['wallClockUsedForFaultSelectionOrEconomics'] is False
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'fault taxonomy, no-blind-retry, deterministic event-time rate limits and reconciliation gate are frozen'

printf '%s\n' '[5/8] C++17 deterministic chaos campaign passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step55_fault_chaos_rate_limit_test.cpp" \
 -o "$TMP/step55_test"
"$TMP/step55_test"
pass 'disconnect/reconnect, stale snapshot, delay/loss, duplicates, out-of-order, rejects, rate limits and seed determinism validated'

printf '%s\n' '[6/8] Chaos/rate-limit decisions use no wall-clock timer or sleep'
if grep -Ein 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|gettimeofday|clock_gettime|std::time|(^|[^[:alnum:]_])time\(' \
 "$ROOT/lib/src/exchange/mock_chaos_prng_v1.h" \
 "$ROOT/lib/src/exchange/mock_fault_chaos_rate_limit_v1.h" >"$TMP/clock"; then
 cat "$TMP/clock" >&2
 fail 'wall/monotonic time leaked into Step55 fault selection/rate-limit behavior'
fi
grep -Fq 'deterministicPpmDrawV1' "$ROOT/lib/src/exchange/mock_fault_chaos_rate_limit_v1.h" || fail 'deterministic seed selector missing'
grep -Fq 'evidenceFingerprint' "$ROOT/lib/src/exchange/mock_fault_chaos_rate_limit_v1.h" || fail 'chaos evidence fingerprint missing'
pass 'fault decisions and rate-limit recovery depend only on seed/input ordinals/supplied event time'

printf '%s\n' '[7/8] Step56/private real-venue wiring has not slipped in'
if grep -Ein 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|smart[_ -]?order|portfolio[_ -]?risk|order[_ -]?planner|strategy.*submit' \
 "$ROOT/lib/src/exchange/mock_fault_chaos_rate_limit_v1.h" \
 "$ROOT/lib/src/exchange/mock_chaos_prng_v1.h" >"$TMP/future"; then
 cat "$TMP/future" >&2
 fail 'Step56/private real-venue/full-system concern leaked into Step55 code'
fi
pass 'Step55 remains an isolated MOCK fault layer; no private/full-system route was enabled'

printf '%s\n' '[8/8] Step56 handoff is bound to exact Step55 fingerprint'
python3 -S - "$CFG/fault_chaos_rate_limit_manifest_v1.json" "$ART/STEP_56_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step55-to-step56-v1'
assert h['nextStep']=='STEP_56_FULL_SYSTEM_REPLAY_RUNTIME'
assert h['step55FaultChaosFingerprint']==m['combinedFaultChaosFingerprint']
assert h['step54ReconciliationLedgerFingerprint']==m['step54ReconciliationLedgerFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert 'wire historical market data through TimeHandler -> Strategy -> PortfolioRisk -> OrderPlanner' in h['requiredStep56Behavior']
PY
pass 'next safe step is Step56 Full-System Replay Runtime'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 55: PASS — FAULT / CHAOS / RATE-LIMIT ENGINE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Deterministic disconnect/reconnect, stale evidence, response ambiguity, duplicate/out-of-order, rejects and rate limits are validated.'
printf '%s\n' 'Unsafe/stale reconciliation still blocks new exposure. No private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 56 Full-System Replay Runtime.'
