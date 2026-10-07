#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step53"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP53: PASS: %s\n' "$*"; }
fail(){ printf 'STEP53: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 53 — MOCK SNAPSHOT / USER STREAM / RECOVERY'
printf '%s\n' '============================================================'
printf '%s\n' 'Durable MOCK recovery only. No real reconciliation/private real-venue routing.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step51 + Step52 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step51/SHA256SUMS >/dev/null) || fail 'Step51 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step52/SHA256SUMS >/dev/null) || fail 'Step52 frozen artifact changed'
pass 'Step51 matching/fills and Step52 accounting remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step53 artifacts are present and hash-clean'
for f in \
 "$CFG/snapshot_recovery_v1.json" "$CFG/snapshot_recovery_manifest_v1.json" \
 "$ART/STEP_53_MOCK_SNAPSHOT_USER_STREAM_RECOVERY.md" \
 "$ART/STEP_54_IMPLEMENTATION_HANDOFF.json" "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock/recovery_codec.h" \
 "$ROOT/lib/src/exchange/mock/recovery.h" \
 "$ROOT/validation/step53_mock_snapshot_user_stream_recovery_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step53 hash mismatch'
pass 'Step53 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step53 fingerprint binds to exact Step52 accounting fingerprint'
python3 -S - "$CFG/accounting_manifest_v1.json" "$CFG/snapshot_recovery_v1.json" "$CFG/snapshot_recovery_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s52=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step53-v1\n{s52['combinedAccountingFingerprint']}\n"
    f"mock-snapshot-user-stream-recovery-v1-step53\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep52AccountingFingerprint']==s52['combinedAccountingFingerprint']
assert m['step52AccountingFingerprint']==s52['combinedAccountingFingerprint']
assert m['recoveryConfigSha256']==sha
assert m['combinedRecoveryFingerprint']==fp
assert m['durableCheckpointImplemented'] is True
assert m['incrementalJournalImplemented'] is True
assert m['userStreamImplemented'] is True
assert m['boundedBackfillImplemented'] is True
assert m['restartRecoveryImplemented'] is True
assert m['postRestartDedupImplemented'] is True
assert m['outOfOrderFailClosedImplemented'] is True
assert m['realVenueReconciliationImplemented'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_54_RECONCILIATION_AND_LEDGER_PARITY'
PY
pass 'recovery fingerprint is reproducible and bound to Step52'

printf '%s\n' '[4/8] Snapshot/journal/stream semantics are explicit and fail-closed'
python3 -S - "$CFG/snapshot_recovery_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['persistenceModel']=='DURABLE_DETERMINISTIC_REPLAY_CHECKPOINT_PLUS_INCREMENTAL_JOURNAL'
assert d['checkpoint']['journalTruncatedOnlyAfterCheckpointCommit'] is True
assert d['incrementalJournal']['appendPolicy']=='WRITE_AHEAD_FSYNC_BEFORE_APPLY'
assert d['incrementalJournal']['corruptOrTruncatedRecord']=='FAIL_CLOSED'
assert d['userStream']['sequenceStartsAt']==1
assert d['userStream']['maxBackfillEventsPerPage']==256
assert d['userStream']['cursor']=='LAST_DELIVERED_SEQUENCE'
assert d['dedup']['sameIdentitySamePayload']=='DUPLICATE_IGNORED_FOR_NON_COMMAND_SOURCE_EVENTS'
assert d['dedup']['sameIdentityDifferentPayload']=='RECOVERY_UNSAFE_FAIL_CLOSED'
assert d['outOfOrderPolicy']['newSourceEventWithEventTimeLessThanLastAcceptedEventTime']=='REJECT_AND_MARK_RECOVERY_UNSAFE'
assert d['outOfOrderPolicy']['checkpointAllowedWhileUnsafe'] is False
assert d['wallClockUsedForEconomics'] is False
assert d['realVenueReconciliationImplemented'] is False
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'checkpoint + incrementals + bounded stream/backfill + unsafe-state policy are frozen'

printf '%s\n' '[5/8] C++17 restart/snapshot/backfill/dedup/out-of-order suite passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/exchange" -I"$ROOT/lib/src/data_types" \
 "$ROOT/validation/step53_mock_snapshot_user_stream_recovery_test.cpp" \
 -o "$TMP/step53_test"
"$TMP/step53_test"
pass 'restart restores exact economics/stream; snapshot+incrementals and post-restart dedup validated'

printf '%s\n' '[6/8] Durable writes are fsync-based while economic time remains supplied event time'
grep -Fq '::fsync' "$ROOT/lib/src/exchange/mock/recovery_codec.h" || fail 'fsync durability missing'
grep -Fq 'durableAppendFramed' "$ROOT/lib/src/exchange/mock/recovery_codec.h" || fail 'durable journal append missing'
grep -Fq 'durableWriteAtomic' "$ROOT/lib/src/exchange/mock/recovery_codec.h" || fail 'atomic checkpoint writer missing'
if grep -Ein 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|gettimeofday|clock_gettime|std::time|(^|[^[:alnum:]_])time\(' \
 "$ROOT/lib/src/exchange/mock/recovery.h" >"$TMP/clock"; then
 cat "$TMP/clock" >&2
 fail 'wall/monotonic time leaked into Step53 business/economic sequencing'
fi
pass 'technical durability is real; business/event time remains replay-supplied'

printf '%s\n' '[7/8] Step54/private real-venue implementation has not slipped in'
if grep -Ein 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|smart[_ -]?order|real[_ -]?venue[_ -]?reconciliation' \
 "$ROOT/lib/src/exchange/mock/recovery.h" \
 "$ROOT/lib/src/exchange/mock/recovery_codec.h" >"$TMP/future"; then
 cat "$TMP/future" >&2
 fail 'Step54/private real-venue concern leaked into Step53 code'
fi
pass 'MOCK recovery is isolated from real venue reconciliation/auth/routing'

printf '%s\n' '[8/8] Step54 handoff is bound to exact Step53 fingerprint'
python3 -S - "$CFG/snapshot_recovery_manifest_v1.json" "$ART/STEP_54_IMPLEMENTATION_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step53-to-step54-v1'
assert h['nextStep']=='STEP_54_RECONCILIATION_AND_LEDGER_PARITY'
assert h['step53RecoveryFingerprint']==m['combinedRecoveryFingerprint']
assert h['step52AccountingFingerprint']==m['step52AccountingFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert 'ledger fill chain parity against canonical Fill authority' in h['requiredStep54Behavior']
PY
pass 'next safe step is Step54 Reconciliation + Ledger Parity'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 53: PASS — MOCK SNAPSHOT / USER STREAM / RECOVERY VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Durable checkpoint+journal recovery, deterministic user stream/backfill and post-restart dedup are validated.'
printf '%s\n' 'No real reconciliation/private real-venue routing was enabled.'
printf '%s\n' 'Next safe step: Step 54 Reconciliation + Ledger Parity.'
