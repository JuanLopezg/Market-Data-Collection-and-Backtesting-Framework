# algoTrading

C++/Python trading research, a reusable trading library, live services and an
observational dashboard. Current validated execution is canonical historical
replay through a deterministic MOCK venue. Private exchange execution remains
unfinished.

## Start here

- [Current state and accepted evidence](CURRENT_STATE.md)
- [Remaining roadmap and user requirements](docs/ROADMAP.md)
- [Documentation index](docs/README.md)
- [Architecture](docs/codex/ARCHITECTURE_FULL.md)
- [Library](lib/README.md), [live services](live_trading/README.md),
  [replay](research/REPLAY.md), [dashboard](dashboard/README.md)
- [Validation guide](validation/README.md)

For coding work, read [AGENTS.md](AGENTS.md), then the two context/architecture
files it names and the relevant README. Source and current test output take
precedence over documentation.

Generate local file/component navigation with `python3 tools/generate_ai_index.py`.
Open `.ai/README.md`, and check freshness with
`python3 tools/generate_ai_index.py --check`. The generated directory stays ignored
by Git. See the [index guide](docs/codex/AI_INDEX.md) for scope and exclusions.

## Replay

Use the one public entrypoint:

```bash
python3 research/replay.py system --days 107 --label system_107d
python3 research/replay.py dashboard --days 107 --label dashboard_107d --no-dashboard-up
python3 research/replay.py fast --days 107 --label fast_107d
```

`fast` uses the current library Backtester through the research entrypoint.
`system` uses the canonical Strategy/Risk/Planner/MOCK runtime.
`dashboard` shares system economics and publishes dashboard state.

`--start` starts strategy state and warm-up at that date. To inspect a later
period with earlier history preserved, use `--pace-start/--pace-end`.
See [replay controls and restart examples](research/REPLAY.md).

The RealTest parity profile preserves original OHLCV, decides USD targets at
close(T), resolves quantities at open(T+1), and uses full next-open fills with
zero fees/slippage and no volume-capacity limit. Normal MOCK realism is separate.
Differences are reported directly; they are never accepted through a whitelist.

## Build and validate

Use WSL from the repository root:

```bash
# For a new build directory:
meson setup build
meson compile -C build -j 3
bash validation/step59_canonical_replay_release_gate.sh
```

Skip setup when `build` is already configured. The compact gate checks 107 days,
25/25 RealTest matches, zero differences, 50 fills, determinism, system/dashboard
restart and pacing invariance. Full-history and broker acceptance are separate.

## Deployment and handoff

[Live deployment](deploy/live/README.md) and
[distributed historical deployment](deploy/historical_replay/README.md) describe
their respective Compose stacks. Dashboard deployment has its own
[guide](dashboard/docs/AWS_DEPLOYMENT.md). None of these guides constitute
approval to route live capital.

`./handoff.sh` packages source and current documentation. Generated datasets,
replay output and frozen evidence are not substitutes for current source checks.

## Files kept local

`.gitignore` excludes generated runtime bundles, replay output/checkpoints,
mutable dashboard state, compiled runners, logs, dependency caches and local credentials.
These files can remain on disk without being included in a commit. Original
`.env.example` templates, source configuration, the canonical input dataset and
frozen historical specifications/summary evidence remain versioned.

Recreate deployment bundles with their `build_runtime_bundle.sh` scripts after
building in WSL. New machines copy environment templates and supply local credentials.
Ignored datasets under `storage/` still need to be provided locally for validation.
Removing a file from current Git tracking does not remove it from earlier commits.
