# Current state

Last synchronized: 2026-10-07. Current source and test output take precedence.

## Latest validated baseline

The full WSL Meson build passed with 22 configured targets after library and
live-service readability cleanup. Target names and binary output paths were
preserved. The compact Step59 gate passed:

```text
window             2020-01-01..2020-04-16
days               107
RealTest matches   25/25
differences        0
canonical fills    50
fingerprint        94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

It covers deterministic reruns, system restart at a day boundary, dashboard
equality/restart, and pacing-speed invariance. Existing live structural/clock/
SQLite/identity/deployment audits and historical no-lookahead tests also passed.

## Documentation/validation cleanup verified

On 2026-10-07 the WSL project build and Step59 gate were rerun successfully with the
same compact fingerprint above. Current LIVE structural/clock/daily/SQLite/identity/
deployment checks, the no-legacy-clock audit, historical topology/no-shared-clock
checks and the historical visibility no-lookahead unit test passed.

The dashboard source checker passed 33 contract markers. All 34 API routes are covered
by the updated guide; retained Markdown links resolve. Syntax checks passed for 86
validation/dashboard/handoff shell scripts and the modified Python checkers. The moved
dashboard source guide retains the ledger gate's required contract markers.

Removed 39 obsolete files (10 documentation files), moved five dashboard guides into
`dashboard/docs/` and renamed the current absence audit to
`validation/no_legacy_clock_audit.sh`. Current explanation text was consolidated
into the retained guides and `docs/README.md` index. The historical topology audit
now checks current read-only/query-only consumer ownership while allowing WAL sidecar
access on the existing writable named volume.

Frozen venue document bytes and generated evidence were preserved. No trading algorithm,
configuration or deployed topology was changed by this documentation cleanup.
Browser/full-history/deployed-runtime campaigns were not rerun.

## Earlier evidence, not rerun by the readability cleanup

Full history: `2020-01-01..2025-10-13`, 2113 source days, 631 candidate
trades, 628 fully matched, four comparison differences, 1261 fills.

```text
1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

The differences are the BNB exit date, missing candidate FET trade, final ZEC
end-state/time, and extra candidate FET trade. They remain visible for human
review; there is no mismatch whitelist. Earlier full-history dashboard/system
fills were byte-identical.

A warmed later-date pacing proof recorded:

```text
b85026fe0cf5206dc4120af9b3a12cdc6a2a978d08faf69b0dc99b602bbe9024
```

These recorded proofs do not imply that every legacy validation script is usable.

## Current code and readiness

- Current library filenames are descriptive; supported source no longer uses
  version suffixes. Wire/schema/version labels remain unchanged.
- PureRSI is the only supported strategy under `lib/src/strategy/validated/`.
  `on_hold/` experiments are excluded from the supported build.
- Live services retain cohesive state machines and meaningful local components.
  There is one source-level build file per service.
- `research/replay.py {fast|system|dashboard}` is the public replay interface.
- Six historical research executables still depend on
  `research/src/legacy/runtime/`. This is temporary compatibility code.
- Dashboard private routing remains blocked. Public venue metadata does not
  establish private account/order connectivity.
- Slow browser acceptance, deployed NATS/PostgreSQL integration, VPS equivalence,
  private TESTNET/shadow and MAINNET readiness remain separate outstanding work.

## Next task

Inventory the six frozen-runtime research programs, preserve their useful
reporting/experiment capabilities, and migrate them to current library APIs.
Start with `research/src/legacy/backtesting_metrics_main_html_reports.cpp`.
Remove the frozen runtime only after its remaining useful consumers are migrated.

See the [handoff](docs/codex/CONTEXT_FULL.txt), [architecture](docs/codex/ARCHITECTURE_FULL.md)
and [validation guide](validation/README.md) for navigation and checks.
