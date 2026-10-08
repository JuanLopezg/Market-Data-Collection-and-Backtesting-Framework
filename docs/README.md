# Documentation index

Current operational guides are listed below. They describe current behavior;
numbered evidence and generated reports serve different purposes.

## Read in this order for code work

1. [Repository instructions](../AGENTS.md)
2. [Current handoff](codex/CONTEXT_FULL.txt)
3. [Architecture and source map](codex/ARCHITECTURE_FULL.md)
4. The relevant component README and actual source.

[Current state](../CURRENT_STATE.md) owns the accepted evidence summary.
The handoff owns priorities and constraints; architecture owns navigation.
Neither is a cumulative development diary.

## Current guides

| Area | Guide |
| --- | --- |
| Project entrypoint and build | [Root README](../README.md) |
| Ordered remaining work and user requirements | [Roadmap](ROADMAP.md) |
| Library domains and reading order | [lib](../lib/README.md) |
| Live service ownership, timing and durability | [live_trading](../live_trading/README.md) |
| Replay modes, cutoff, pacing and resume | [research/replay](../research/REPLAY.md) |
| Tests, structural audits and integration checks | [validation](../validation/README.md) |
| Controlled local service/recovery environment before renting a VPS | [Local campaign](../validation/LOCAL_SERVICE_CAMPAIGN.md) |
| Generated local repository navigation | [AI index](codex/AI_INDEX.md) |
| Dashboard providers and local use | [dashboard](../dashboard/README.md) |
| Dashboard HTTP boundary | [API contract](../dashboard/docs/API_CONTRACT.md) |
| Dashboard source ownership | [Source map](../dashboard/docs/REAL_DATA_SOURCE_MAP.md) |
| Dashboard access controls | [Authentication](../dashboard/docs/AUTH_SECURITY.md) |
| Dashboard VPS deployment | [Deployment](../dashboard/docs/AWS_DEPLOYMENT.md) |
| Dashboard production review | [Checklist](../dashboard/docs/PRODUCTION_CHECKLIST.md) |
| Live Compose stack | [Live deploy](../deploy/live/README.md) |
| Current-data paper stack and dashboard host telemetry | [Paper trading](../deploy/paper_trading/README.md) |
| Isolated distributed historical stack | [Historical deploy](../deploy/historical_replay/README.md) |
| Sensitivity CSV analysis | [Report tool](../tools/README_sensitivity_report.md) |

## Retained evidence and compatibility material

`docs/venue/step47a..step58a/` contains venue specifications, design freezes,
handoffs and accepted evidence. Numbered gates/configuration refer to these
files and their `SHA256SUMS`. Their captured source paths may predate the
readability refactor. Keep the evidence bytes and paths intact; use current
architecture/source for navigation. Existing historical checksum drift must be
investigated, not hidden by regenerating hashes.

`config/venues/mock/` includes catalog/rule manifests and a source registry.
These `.txt`/JSON/CSV files are runtime or integrity inputs, not disposable prose.
`config/historical_replay/runtime_manifest.txt` is a packaging input.

The research consumer inventory and CURRENT reporting workflows are maintained in
`research/REPLAY.md`. The frozen research runtime was removed after all useful
consumers migrated; accepted migration evidence remains in `CURRENT_STATE.md`.

`storage/` and `deploy/historical_replay/run/` contain datasets, user reports,
accepted replay output and generated evidence/logs. They are not the current
documentation tree and were not pruned by this cleanup. Historical tools under
`tools/historical_replay/` may still depend on these captures.

## Cleanup policy

Remove duplicated implementation diaries and checks for deleted infrastructure.
Keep useful regression tests, contracts, runbooks and evidence.
When a guide moves, update its links and any scripts that read it.
Historical references are not proof of current readiness. No documentation edit
authorizes deployment, a private exchange action, or a change in trading economics.
