# Deployment coordination — 2026-10-18

Deployment entrypoint: `deploy/chain/compose.yml` and the installed `chain/algo`
launcher; see [chain operations](../../deploy/chain/README.md). Standalone dashboard
deployment is retired. The completed release notes below are retained evidence;
this cleanup does not deploy or activate a release.

Owner: Codex, current user-authorized Telegram formatting / Kraken daily table task.
Status: COMPLETE — color/name-only summary release activated and verified; deployment slot free.
Latest release: `20261018_130842-9bed096-dirty`, previous `20261018_121047-9bed096-dirty`.
Activated at 13:13 UTC: all 13 containers healthy, startup reconciliation clean,
3 positions / 3 fills / stops 7/8/9 preserved; running image IDs match the manifest.
Concurrent valuation edits were excluded using an isolated baseline source tree and API
rebuild. Local package `20261018_130038-9bed096-dirty` is NOT_FOR_DEPLOYMENT, never uploaded.
Backup: `/algoTrading/backups/20261018_131316.tar.gz`. No manual orders or test Telegram sends.
The following notes describe the preflight and the earlier daily-table deployment.
Preflight 12:44 UTC: current `20261018_121047-9bed096-dirty`, all 13 containers healthy,
no competing activation/build detected. Abort activation if current changes meanwhile.

Observed VPS current release at 11:54 UTC: `20261018_114445-9bed096-dirty`.
All 13 containers healthy; no active release/build/load process detected on VPS.
Local runtime/compose launcher match the deployed release. Pending changes concern
Telegram alert presentation and the read-only Kraken daily portfolio table.

The user explicitly authorized deployment, then requested the daily portfolio table
as part of that ongoing work. No manual real orders or test Telegram sends are authorized.
Activation must stay outside 23:30–00:30 UTC. Verify the current release again before
activation and abort if another session has changed it.

Final release: `20261018_121047-9bed096-dirty`, previous `20261018_114445-9bed096-dirty`.
All 13 containers healthy, execution startup reconciliation clean, 3 positions / 3 fills /
accepted stops 7/8/9 preserved. Running image IDs match the freshly built manifest.
Kraken read-only monitor restarted, healthy, daily snapshot history retained and rendered
with the installed table formatter. No manual real order or test Telegram message sent.
See CURRENT_STATE.md for validation, image IDs and backup evidence.
