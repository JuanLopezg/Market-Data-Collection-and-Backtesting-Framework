# Behaviour tests (GoogleTest + meson test)

These tests check that `lib/` and the C++ services that run on the VPS (market data,
strategy, risk, planner, execution state, exchange gateway, simulated exchange) do what
the trading system is supposed to do. They are not a mirror of the current code.

```bash
ninja -C build
meson test -C build --suite unit       # in-process, seconds
meson test -C build --suite service    # one real service binary + Docker NATS/PostgreSQL
meson test -C build --suite chain      # the whole chain over several business days
meson test -C build                    # everything
```

Service and chain tests start throwaway `nats:2.10-alpine` and `postgres:16-alpine`
containers on random loopback ports and skip themselves when Docker is unavailable.
`ALGOTRADING_TEST_KEEP_TMP=1` keeps each test's temporary folder (logs, SQLite files).
The existing release gate (`validation/step59_canonical_replay_release_gate.sh`) stays
the accepted RealTest/economic fingerprint; these suites do not replace it.

## Layout

| Folder | What it holds |
| --- | --- |
| `specs/` | One behaviour specification per area: what the code must do, with worked examples. |
| `support/` | `test_support.*`: temp folders, child processes, Docker NATS/PostgreSQL. |
| `unit/<area>/` | In-process tests of one `lib/` area or one service's internal library. |
| `service/<area>/` | Black-box tests of one service executable: messages in, messages/state out. |
| `chain/` | The trading chain end to end over several simulated days. |

Each area folder owns its `meson.build`. A test executable is registered like this:

```meson
test('unit_planning',
    executable('test_unit_planning', files('notional_planner_test.cpp', ...),
               dependencies: test_support_dep),
    suite: ['unit'], env: test_env, protocol: 'gtest')
```

Service and chain tests add `depends: service_depends`, a `timeout:` and use
`test_support::builtExecutable("ALGOTRADING_BIN_<NAME>")` for the binaries.
Service internals are linkable as `market_data_core_dep`, `execution_state_core_dep`
and `exchange_gateway_venue_dep`.

## How tests are written here

The common failure of generated tests is that they restate the implementation: read a
function, assert whatever it returns today. Such tests pass on bugs and only break on
refactors. Every area here is done in this order instead:

1. **Specification first** (`specs/<area>.md`). Describe what the area must do from the
   point of view of the trading system: inputs, outputs, invariants, timing rules
   (what is known at close(T), what happens at open(T+1)), error handling, restart and
   duplicate behaviour. Sources, in order: `docs/codex/*`, the READMEs, the public
   headers and their comments, contracts and configs, and trading domain knowledge.
   Each rule gets at least one worked example with numbers computed by hand.
   Where the docs do not say, write the behaviour a correct trading system needs and
   mark it `(assumed)`.
2. **Tests from the specification.** Each test names the spec rule it checks. Expected
   values come from the worked examples or from invariants, never from running the
   code and copying its output.
3. **Disagreements are findings, not chores.** When a test fails, decide whether the
   test misread the spec (fix the test) or the code disagrees with the spec. Never
   change an expected value only because the code returns something else. A code/spec
   disagreement stays visible: the test is kept, prefixed `DISABLED_`, with a
   `// FINDING:` comment, and it is listed in the area's spec under "Findings".

What a good test looks like here:

- **Behaviour, not structure.** Drive the public API (or the service's messages) the way
  the real system does; assert outcomes the trading system cares about (positions,
  cash, PnL, orders, published messages, durable rows), not private call sequences.
- **Invariants.** Cash + position value moves only by fills, fees and funding; realized
  PnL equals the sum of closed trades; a position's sign matches its side; no order
  without a decision, no fill without an order; nothing at close(T) uses data after T.
- **Same result under changed conditions.** Re-delivering a message, restarting a
  service or reordering independent inputs must not change the economic result.
- **Edge cases that matter for money.** Partial fills, sign flips, zero/negative
  quantities, missing prices, unknown symbols, duplicate IDs, stale or future data.
- **Integration over isolation.** Prefer real collaborators (real `lib` classes, real
  SQLite, real NATS/PostgreSQL) over mocks. Mock only the outside world (Binance,
  Kraken HTTP).
- Deterministic: no wall-clock dependence (use `TimeHandler` settings), no Internet.
