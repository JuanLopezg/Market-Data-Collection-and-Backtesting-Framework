# algoTrading — Codex instructions

Before substantial work, read in this order:

1. `docs/codex/CONTEXT_FULL.txt`
2. `docs/codex/ARCHITECTURE_FULL.md`
3. the relevant active README (`lib/README.md`, `live_trading/README.md`, `research/REPLAY.md`)
4. the actual source files involved in the task.

The source code and current test/build output are the final source of truth.
Use [docs/README.md](docs/README.md) to find other maintained guides. Validation evidence
belongs in CURRENT_STATE.md; avoid duplicating historical step diaries in active guides.

## Project style

- Optimize for readability and understandability, not minimum line count.
- Comments, logs, identifiers and technical errors are in English.
- Avoid clever abstractions.
- Avoid both giant multi-responsibility files and pointless fragmentation.
- Do not create wrapper-only files just to make `main()` tiny.
- A moderately long `main()` is acceptable when it contains real orchestration.
- Extract components only for meaningful responsibility/state/lifecycle/reuse boundaries.
- Preserve trading behavior during readability-only refactors.
- Keep important timing and economic rules explicit.
- Never log secrets.

## Trading safety

For strategy/risk/sizing/planning/execution/fill/accounting changes, trace:

`market data -> state/features -> strategy -> risk -> sizing/planning -> execution -> order lifecycle -> matching/fill -> position/accounting/PnL -> reconciliation`

Check:
- look-ahead / leakage,
- duplicate signals/orders/fills,
- idempotency,
- timestamp ordering,
- position sign,
- partial fills,
- realized/unrealized PnL,
- restart/resume,
- reconciliation drift,
- live/replay differences.

## Validation

Use WSL.

For meaningful `lib/`, canonical, execution, exchange, accounting, recovery, or cross-cutting changes:

```bash
bash validation/step59_canonical_replay_release_gate.sh
```

Expected compact fingerprint:

```text
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

An unexpected fingerprint change is a regression until explained.

Do not run full-history/slow visual tests unless the task requires them.

## Current direction

1. Keep current library/live structure readable; finish only concrete remaining issues.
2. Migrate `research/` to CURRENT `lib/` APIs and behavior.
3. Remove `research/src/legacy/runtime/` only after useful research tools are migrated.
4. Build the generated `.ai/` context/index layer.
5. Continue VPS and exchange-readiness work using `docs/ROADMAP.md`: compare Kraken/Hyperliquid coverage and BTC collateral first; Kraken is preferred for the initial small live test, subject to validation.

Do not treat the frozen research legacy runtime as permanent architecture.
