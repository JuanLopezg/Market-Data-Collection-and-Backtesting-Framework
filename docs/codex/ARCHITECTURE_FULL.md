# algoTrading — Full architecture and navigation graph

This file is intentionally detailed. It is a map for Codex or a developer who is new to the repository.

**Important:** diagrams describe the known current architecture and validated flows. Source code remains the source of truth.

---

# 1. Repository-level map

```mermaid
flowchart TB
    ROOT["algoTrading/"]

    ROOT --> LIB["lib/<br/>Current reusable trading library"]
    ROOT --> LIVE["live_trading/<br/>Executable services"]
    ROOT --> RESEARCH["research/<br/>Research + canonical replay"]
    ROOT --> DASH["dashboard/<br/>Dashboard / UI integration"]
    ROOT --> VALID["validation/<br/>Parity and release gates"]
    ROOT --> DEPLOY["deploy/<br/>Historical replay / deployment"]
    ROOT --> STORAGE["storage/<br/>Historical datasets + accepted evidence"]
    ROOT --> DOCS["docs/<br/>Current documentation / Codex context"]

    LIB --> COMMON["common_types / data_types / utils"]
    LIB --> MARKET["market / indicator / universe / ranker / filter"]
    LIB --> STRATEGY["strategy / signal"]
    LIB --> RISK["risk / sizing / portfolio / rebalance"]
    LIB --> EXEC["execution / exchange"]
    LIB --> RUNTIME["runtime"]
    LIB --> STATE["persistence / recovery"]
    LIB --> CONTRACTS["contracts / transport"]
    LIB --> RESEARCHLIB["backtest / analytics"]
```

---

# 2. Library domain map

```mermaid
flowchart LR
    DATA["Market & domain data<br/>common_types<br/>data_types<br/>market"]
    FEATURES["Features / selection<br/>indicator<br/>universe<br/>ranker<br/>filter"]
    SIGNALS["Strategy / signals<br/>strategy<br/>signal"]
    CAPITAL["Capital decisions<br/>risk<br/>sizing<br/>portfolio<br/>rebalance"]
    ORDERS["Execution intent<br/>execution"]
    VENUE["Venue mechanics<br/>exchange"]
    ORCH["Orchestration<br/>runtime"]
    DURABLE["Durability<br/>persistence<br/>recovery"]
    WIRE["Contracts / wire formats<br/>contracts<br/>transport"]
    TESTING["Research support<br/>backtest<br/>analytics<br/>testing"]

    DATA --> FEATURES
    FEATURES --> SIGNALS
    SIGNALS --> CAPITAL
    CAPITAL --> ORDERS
    ORDERS --> VENUE

    ORCH --> DATA
    ORCH --> FEATURES
    ORCH --> SIGNALS
    ORCH --> CAPITAL
    ORCH --> ORDERS
    ORCH --> VENUE

    VENUE --> DURABLE
    WIRE --> ORCH
    WIRE --> VENUE

    DATA --> TESTING
    SIGNALS --> TESTING
    CAPITAL --> TESTING
```

---

# 3. Economic trading flow

```mermaid
flowchart LR
    BAR["Market bar / observation"]
    ROLL["Rolling market state"]
    IND["Indicators / features"]
    UNIV["Universe / ranking"]
    STRAT["Strategy decision"]
    RISK["Risk approval / target"]
    SIZE["Sizing / monetary target"]
    PLAN["Order planning"]
    EXEC["Execution engine"]
    ADAPTER["Exchange adapter"]
    LIFE["Order lifecycle"]
    MATCH["Matching"]
    FILL["Fill"]
    POS["Position state"]
    ACCT["Cash / margin / accounting"]
    PNL["Realized + unrealized PnL"]
    RECON["Recovery / reconciliation"]

    BAR --> ROLL
    ROLL --> IND
    ROLL --> UNIV
    IND --> STRAT
    UNIV --> STRAT
    STRAT --> RISK
    RISK --> SIZE
    SIZE --> PLAN
    PLAN --> EXEC
    EXEC --> ADAPTER
    ADAPTER --> LIFE
    LIFE --> MATCH
    MATCH --> FILL
    FILL --> POS
    FILL --> ACCT
    POS --> PNL
    ACCT --> PNL
    ACCT --> RECON
    LIFE --> RECON
```

---

# 4. Canonical replay entry path

```mermaid
flowchart TB
    USER["CLI user"]
    PY["research/replay.py"]
    CPP["research/src/canonical/canonical_replay.cpp"]
    FSR["lib/src/runtime/full_system_replay_runtime_v1.*"]

    USER -->|"system / dashboard"| PY
    PY -->|"standalone compile + execute"| CPP
    CPP --> FSR

    FSR --> SSE["StrategySignalEngine"]
    FSR --> PRE["PortfolioRiskEngine"]
    FSR --> OPE["Order planning"]
    FSR --> MOCK["MockExchangeAdapterV1"]
    FSR --> EVID["Evidence / fingerprint"]
    FSR --> DSTATE["Dashboard state publication"]
```

Important build detail:

```mermaid
flowchart LR
    NEWSRC["Previously header-only implementation<br/>becomes new .cpp"]
    MESON["Add to Meson"]
    REPLAYPY["Add to research/replay.py<br/>standalone canonical source list"]
    BUILD["Normal project build"]
    STEP59["Step59 standalone canonical build"]

    NEWSRC --> MESON --> BUILD
    NEWSRC --> REPLAYPY --> STEP59
```

A Meson PASS alone does not prove the standalone Step59 compile source list is complete.

---

# 5. Canonical economic validation

```mermaid
flowchart LR
    OHLCV["Historical OHLCV"]
    CLOSE["CLOSE(T)<br/>determine monetary target"]
    NEXTOPEN["OPEN(T+1)<br/>resolve target quantity"]
    FULLFILL["Full fill at next open<br/>parity mode"]
    TRADES["Candidate trades"]
    REAL["RealTest CSV"]
    COMP["Direct comparison"]
    FP["Deterministic fingerprint"]

    OHLCV --> CLOSE
    CLOSE --> NEXTOPEN
    NEXTOPEN --> FULLFILL
    FULLFILL --> TRADES
    TRADES --> COMP
    REAL --> COMP
    TRADES --> FP
```

Parity constraints:

```text
slippage = 0
fees = 0
no volume capacity
no synthetic 1e10 / 1e11 liquidity
no hidden exception whitelist
```

---

# 6. Step59 release gate

```mermaid
flowchart TB
    START["Step59"]
    BUILD["1. Compile active canonical runner"]
    PARITY["2. 107-day RealTest parity<br/>25/25, 0 differences, 50 fills"]
    DET["3. Repeated deterministic run"]
    SYSRES["4. System checkpoint/resume"]
    DASHEQ["5. Dashboard == system<br/>and dashboard restart-safe"]
    PACE["6. Pacing speed invariance"]
    CONS["7. Internal evidence consistency"]
    FP["Accepted fingerprint<br/>94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2"]
    PASS["Release-candidate gate PASS"]
    STOP["STOP and investigate"]

    START --> BUILD --> PARITY --> DET --> SYSRES --> DASHEQ --> PACE --> CONS --> FP
    FP -->|"matches"| PASS
    FP -->|"unexpected change"| STOP
```

---

# 7. Full-history evidence

```mermaid
flowchart LR
    HIST["2020-01-01 .. 2025-10-13<br/>2113 days"]
    RUN["Canonical full-history replay"]
    CAND["631 candidate trades"]
    MATCH["628 fully matched"]
    DIFF["4 visible differences"]
    FILLS["1261 canonical fills"]
    FP["1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0"]

    HIST --> RUN
    RUN --> CAND
    RUN --> FILLS
    CAND --> MATCH
    CAND --> DIFF
    RUN --> FP
```

Do not hide the four known differences in implementation.

---

# 8. MOCK exchange overview

```mermaid
flowchart TB
    ADAPTER["mock_exchange_adapter_v1"]
    ADMISSION["mock_order_admission_lifecycle_v1.*"]
    MATCHING["mock_deterministic_matching_fill_v1.*"]
    ACCOUNTING["mock_account_margin_positions_accounting_v1.*"]
    SNAPSHOT["mock_snapshot_user_stream_recovery_v1.*"]
    CHAOS["mock_fault_chaos_rate_limit_v1.*"]
    RECON["mock_reconciliation_ledger_parity_v1.*"]
    CODEC["mock_recovery_codec_v1.*"]

    ADAPTER --> ADMISSION
    ADAPTER --> MATCHING
    ADAPTER --> ACCOUNTING
    ADAPTER --> SNAPSHOT
    ADAPTER --> CHAOS

    MATCHING --> ADMISSION
    MATCHING --> ACCOUNTING

    SNAPSHOT --> CODEC
    CHAOS --> RECON
    RECON --> SNAPSHOT
    RECON --> ACCOUNTING
```

---

# 9. Order lifecycle path

```mermaid
flowchart LR
    INTENT["Submit / cancel / modify intent"]
    VALIDATE["Admission validation"]
    IDEM["Request / idempotency handling"]
    STORE["Stored order state"]
    TRANS["Lifecycle transition"]
    UPDATE["Order update event"]
    MATCH["Matching eligibility"]
    FILL["Fill event"]
    FINAL["Terminal / remaining state"]

    INTENT --> VALIDATE
    VALIDATE --> IDEM
    IDEM --> STORE
    STORE --> TRANS
    TRANS --> UPDATE
    UPDATE --> MATCH
    MATCH --> FILL
    FILL --> TRANS
    TRANS --> FINAL
```

Start here for:
- duplicate orders,
- invalid lifecycle transitions,
- idempotency bugs,
- submit/cancel/modify behavior.

Primary component:

```text
lib/src/exchange/mock_order_admission_lifecycle_v1.*
```

---

# 10. Deterministic matching / fill path

```mermaid
flowchart LR
    ORDER["Admitted stored order"]
    BAR["MarketBarObservationV1"]
    LAT["Latency condition"]
    CROSS["Arrival / limit touch"]
    LIQ["Available synthetic/defined liquidity"]
    SLIP["Deterministic slippage rule"]
    PRICE["Synthetic fill price"]
    FILL["Fill emitted"]
    LIFE["Lifecycle cumulative / remaining"]
    ACCT["Accounting update"]

    ORDER --> LAT
    BAR --> LAT
    LAT --> CROSS
    CROSS --> LIQ
    LIQ --> SLIP
    SLIP --> PRICE
    PRICE --> FILL
    FILL --> LIFE
    FILL --> ACCT
```

Primary component:

```text
lib/src/exchange/mock_deterministic_matching_fill_v1.*
```

---

# 11. Accounting / position path

```mermaid
flowchart TB
    FILL["Fill"]
    POSITION["Position quantity / direction"]
    CASH["Cash units"]
    MARGIN["Margin / leverage"]
    REAL["Realized PnL"]
    UNREAL["Unrealized PnL"]
    EQUITY["Equity / account state"]
    SNAP["Snapshot / reconciliation"]

    FILL --> POSITION
    FILL --> CASH
    FILL --> REAL
    POSITION --> UNREAL
    CASH --> EQUITY
    REAL --> EQUITY
    UNREAL --> EQUITY
    MARGIN --> EQUITY
    EQUITY --> SNAP
```

Primary component:

```text
lib/src/exchange/mock_account_margin_positions_accounting_v1.*
```

For PnL bugs verify:
- sign,
- entry basis,
- close/reduction semantics,
- partial fills,
- cash movements,
- realized/unrealized split,
- leverage/margin effects.

---

# 12. Recovery / persistence / reconciliation

```mermaid
flowchart LR
    EVENTS["User-stream / order / fill events"]
    SNAP["Recovery snapshot"]
    CODEC["Recovery codec"]
    DISK["Durable files"]
    RESTART["Restart"]
    REPLAY["Replay / restore"]
    LOCAL["Local expected state"]
    VENUE["Recovered venue snapshot"]
    COMP["Reconciliation compare"]
    DRIFT["Mismatch / drift evidence"]

    EVENTS --> SNAP
    SNAP --> CODEC
    CODEC --> DISK
    DISK --> RESTART
    RESTART --> REPLAY
    REPLAY --> VENUE

    EVENTS --> LOCAL
    LOCAL --> COMP
    VENUE --> COMP
    COMP --> DRIFT
```

Primary components:

```text
mock_snapshot_user_stream_recovery_v1.*
mock_recovery_codec_v1.*
mock_reconciliation_ledger_parity_v1.*
```

---

# 13. Fault / chaos / rate-limit layer

```mermaid
flowchart TB
    CALL["Exchange operation"]
    POLICY["Configured deterministic fault/rate policy"]
    RATE["Rate-limit decision"]
    FAULT["Injected fault / delay / recovery condition"]
    REALCALL["Underlying MOCK operation"]
    RECON["Reconciliation trigger"]
    OUT["Operation result"]

    CALL --> POLICY
    POLICY --> RATE
    POLICY --> FAULT
    RATE --> REALCALL
    FAULT --> REALCALL
    REALCALL --> OUT
    FAULT --> RECON
```

Primary component:

```text
lib/src/exchange/mock_fault_chaos_rate_limit_v1.*
```

---

# 14. Runtime engine relationships

```mermaid
flowchart TB
    FULL["FullSystemReplayRuntimeV1"]
    TIME["Time handler"]
    ROLL["RollingMarketState"]
    SIG["StrategySignalEngine"]
    RISK["PortfolioRiskEngine"]
    PLAN["NotionalOrderPlannerEngine / planner"]
    EXEC["Execution engine"]
    VENUE["Exchange adapter"]
    EVID["Replay evidence / finalization"]

    FULL --> TIME
    FULL --> ROLL
    FULL --> SIG
    FULL --> RISK
    FULL --> PLAN
    FULL --> EXEC
    FULL --> VENUE
    FULL --> EVID

    ROLL --> SIG
    SIG --> RISK
    RISK --> PLAN
    PLAN --> EXEC
    EXEC --> VENUE
```

Known runtime files include:

```text
lib/src/runtime/full_system_replay_runtime_v1.*
lib/src/runtime/strategy_signal_engine.*
lib/src/runtime/portfolio_risk_engine.*
lib/src/runtime/notional_order_planner_engine.*
lib/src/runtime/rolling_market_state.*
lib/src/runtime/time_handler.*
lib/src/runtime/trading_engine.*
lib/src/runtime/execution_engine.*
lib/src/runtime/order_planner_engine.*
lib/src/runtime/decision_engine.*
```

Verify exact current names in source before editing.

---

# 15. Strategy-side navigation

```mermaid
flowchart TB
    BASE["lib/src/strategy/strategy.*"]
    ACTIVE["lib/src/strategy/strategies/"]
    PURE["pureRSI.h"]
    XH["xHBreakout.h"]
    XHATR["xHBreakout_atr.h"]
    DON["donchianBreakout.h"]
    ALL["all_strategies.h"]
    ONHOLD["lib/src/strategy/on_hold/<br/>historical / inactive candidates"]

    BASE --> ACTIVE
    ACTIVE --> PURE
    ACTIVE --> XH
    ACTIVE --> XHATR
    ACTIVE --> DON
    ACTIVE --> ALL
    BASE -.-> ONHOLD
```

Important:
- `lib/src/strategy/strategies/` was previously ignored by `.gitignore` by mistake.
- Do not re-ignore it.
- Do not blindly modernize `on_hold/` before deciding whether those strategies are still useful.

---

# 16. Live-trading service map

```mermaid
flowchart LR
    HMDS["historical_market_data_service"]
    MDS["market_data_service"]
    STRATS["strategy_service"]
    RISKS["portfolio_risk_service"]
    PLANS["order_planner_service"]
    EXECS["execution_service"]
    GATE["exchange_gateway"]
    SIM["simulated_exchange_service / binance_simulator"]
    STATE["execution_state_service"]
    DASH["dashboard"]

    HMDS --> STRATS
    MDS --> STRATS
    STRATS --> RISKS
    RISKS --> PLANS
    PLANS --> EXECS
    EXECS --> GATE
    GATE --> SIM
    SIM --> STATE
    STATE --> DASH
```

This is a conceptual service flow. Verify transport topics/contracts in source.

The readability rule for these services is:

```text
main() may contain real orchestration.
Do not recreate *_application.* wrappers solely to shorten main().
```

---

# 17. Research — current state

```mermaid
flowchart TB
    RESEARCH["research/"]
    CAN["src/canonical/<br/>current canonical replay"]
    COMMON["src/common/<br/>metrics / reports / RealTest"]
    LEGACY["src/legacy/<br/>historical research programs"]
    RUNTIME["src/legacy/runtime/<br/>temporary frozen compatibility runtime"]
    TOOLS["src/tools/"]

    RESEARCH --> CAN
    RESEARCH --> COMMON
    RESEARCH --> LEGACY
    RESEARCH --> TOOLS
    LEGACY --> RUNTIME
```

---

# 18. Research — desired migration

```mermaid
flowchart LR
    OLDMAIN["Historical research executable"]
    OLDRT["Frozen legacy runtime"]
    CAP["Useful capability<br/>HTML / metrics / experiment / strategy study"]
    CURRENT["Current lib APIs + current economics"]
    NEW["Modernized research executable"]
    DELETE["Remove frozen legacy runtime<br/>when all useful tools migrated"]

    OLDRT --> OLDMAIN
    OLDMAIN --> CAP
    CAP --> NEW
    CURRENT --> NEW
    NEW --> DELETE
```

Important distinction:

```text
Research functionality should survive.
Old runtime behavior does not need to survive.
```

The user wants research to reflect the current trading system.

---

# 19. Research migration checklist

```mermaid
flowchart TB
    INV["Inventory one legacy executable"]
    PURPOSE["Describe what it actually provides"]
    DEPS["Identify old API dependencies"]
    MAP["Map to current lib APIs"]
    MIG["Implement current version"]
    BUILD["Compile whole project"]
    BEHAV["Validate intended research output"]
    STEP["Run Step59"]
    NEXT["Move to next executable"]
    REMOVE["Remove legacy runtime when none depend on it"]

    INV --> PURPOSE --> DEPS --> MAP --> MIG --> BUILD --> BEHAV --> STEP --> NEXT
    NEXT --> INV
    NEXT --> REMOVE
```

---

# 20. Data / evidence locations

```mermaid
flowchart TB
    STORAGE["storage/"]
    REAL["backtests/final_tests/pureRSI.csv"]
    RUNS["deploy/historical_replay/run/"]
    VALID["validation/"]
    DASHSTATE["dashboard replay state JSON"]
    FP["accepted fingerprints"]

    STORAGE --> REAL
    RUNS --> DASHSTATE
    VALID --> FP
    REAL --> VALID
    DASHSTATE --> VALID
```

Known accepted fingerprints:

```text
Compact 107-day:
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2

Full history:
1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0

Later-date pacing proof:
b85026fe0cf5206dc4120af9b3a12cdc6a2a978d08faf69b0dc99b602bbe9024
```

---

# 21. Replay warm-up semantics

```mermaid
flowchart LR
    START["--start"]
    COLD["Simulation starts cold"]
    ROLL["Rolling indicators / universe / rankers"]
    DIFFER["Mid-history results can differ without warm-up"]

    PSTART["--pace-start / --pace-end"]
    WARM["Earlier history still processed"]
    DISPLAY["Only selected period is paced/displayed"]

    START --> COLD --> ROLL --> DIFFER
    PSTART --> WARM --> DISPLAY
```

Use pace controls to visually inspect a later interval without losing warm state.

---

# 22. How to investigate a duplicate order

```mermaid
flowchart TB
    DUP["Duplicate order observed"]
    SIG["Was strategy signal duplicated?"]
    DEC["Was decision emitted twice?"]
    PLAN["Was same target planned twice?"]
    REQ["Same or different request/idempotency key?"]
    LIFE["Admission lifecycle accepted twice?"]
    REC["Recovery replay reapplied command/event?"]
    FILL["Actually duplicate order or duplicate fill/display?"]

    DUP --> SIG --> DEC --> PLAN --> REQ --> LIFE --> REC --> FILL
```

---

# 23. How to investigate a PnL mismatch

```mermaid
flowchart TB
    PNL["PnL mismatch"]
    FILL["Compare fills first"]
    QTY["Check quantity resolution"]
    PX["Check execution price"]
    SIGN["Check side / position sign"]
    PART["Check partial/reduction handling"]
    FEES["Check fee/slippage mode"]
    BASIS["Check cost basis"]
    REAL["Realized PnL"]
    UNREAL["Unrealized mark"]
    EQ["Cash/equity reconciliation"]

    PNL --> FILL
    FILL --> QTY
    FILL --> PX
    QTY --> SIGN
    PX --> PART
    PART --> FEES
    FEES --> BASIS
    BASIS --> REAL
    BASIS --> UNREAL
    REAL --> EQ
    UNREAL --> EQ
```

---

# 24. How to investigate look-ahead / leakage

```mermaid
flowchart LR
    BAR["Bar T"]
    OBS["What data is visible at decision time?"]
    SIGNAL["Signal timestamp"]
    TARGET["Target monetary value"]
    NEXT["Next bar OPEN(T+1)"]
    QTY["Resolve quantity"]
    FILL["Fill"]
    FUTURE["Any CLOSE/HIGH/LOW from future bar used early?"]

    BAR --> OBS --> SIGNAL --> TARGET --> NEXT --> QTY --> FILL
    OBS --> FUTURE
```

For historical parity specifically:

```text
target money at CLOSE(T)
quantity at OPEN(T+1)
fill at next open
```

---

# 25. Documentation hierarchy

```mermaid
flowchart TB
    AGENTS["AGENTS.md<br/>short operating rules"]
    CONTEXT["docs/codex/CONTEXT_FULL.txt<br/>long state/history/constraints"]
    ARCH["docs/codex/ARCHITECTURE_FULL.md<br/>navigation graphs"]
    READMES["lib/README.md + live_trading/README.md + research/REPLAY.md"]
    SOURCE["Actual source code"]
    TEST["Current build/test output"]

    AGENTS --> CONTEXT
    AGENTS --> ARCH
    CONTEXT --> READMES
    ARCH --> READMES
    READMES --> SOURCE
    SOURCE --> TEST
```

Priority when facts disagree:

```text
current source + current test output
> active docs
> full Codex context
> historical notes
```

---

# 26. Current cleanup sequence

```mermaid
flowchart LR
    LIVE["live_trading readability<br/>done"]
    LIB["lib readability / structure<br/>current"]
    RESEARCH["research modernization<br/>next"]
    AI[".ai repository index"]
    DOCS["sync current docs"]
    VPS["VPS equivalence"]
    HL["Hyperliquid current API"]
    TESTNET["TESTNET"]
    SHADOW["Shadow mode"]
    VISUAL["Long visual acceptance"]
    MAIN["MAINNET hardening"]
    LIVEGO["Minimal-capital LIVE"]

    LIVE --> LIB --> RESEARCH --> AI --> DOCS --> VPS --> HL --> TESTNET --> SHADOW --> VISUAL --> MAIN --> LIVEGO
```

---

# 27. Where to start — quick path table

| Goal | Start here |
|---|---|
| Understand repository rules | `AGENTS.md` |
| Understand current project state | `docs/codex/CONTEXT_FULL.txt` |
| Understand overall architecture | `docs/codex/ARCHITECTURE_FULL.md` |
| Understand library domains | `lib/README.md` |
| Understand live services | `live_trading/README.md` |
| Canonical CLI | `research/replay.py` |
| Canonical C++ entrypoint | `research/src/canonical/canonical_replay.cpp` |
| Full replay orchestration | `lib/src/runtime/full_system_replay_runtime_v1.*` |
| Strategy signal runtime | `lib/src/runtime/strategy_signal_engine.*` |
| Risk runtime | `lib/src/runtime/portfolio_risk_engine.*` |
| Planner runtime | `lib/src/runtime/notional_order_planner_engine.*` / related planner code |
| Active strategies | `lib/src/strategy/strategies/` |
| Indicator calculations | `lib/src/indicator/` |
| Universe/ranking | `lib/src/universe/`, `lib/src/ranker/`, `lib/src/filter/` |
| Execution | `lib/src/execution/` |
| MOCK exchange facade | `lib/src/exchange/mock_exchange_adapter_v1.h` |
| Order admission/lifecycle | `lib/src/exchange/mock_order_admission_lifecycle_v1.*` |
| Deterministic matching/fills | `lib/src/exchange/mock_deterministic_matching_fill_v1.*` |
| Positions/margin/accounting | `lib/src/exchange/mock_account_margin_positions_accounting_v1.*` |
| Snapshot/restart | `lib/src/exchange/mock_snapshot_user_stream_recovery_v1.*` |
| Recovery serialization | `lib/src/exchange/mock_recovery_codec_v1.*` |
| Fault/rate-limit behavior | `lib/src/exchange/mock_fault_chaos_rate_limit_v1.*` |
| Reconciliation | `lib/src/exchange/mock_reconciliation_ledger_parity_v1.*` |
| Wire/JSON contracts | `lib/src/contracts/`, `lib/src/transport/` |
| RealTest baseline | `storage/backtests/final_tests/pureRSI.csv` |
| Release validation | `validation/step59_canonical_replay_release_gate.sh` |
| Historical research programs | `research/src/legacy/` |
| Temporary compatibility runtime | `research/src/legacy/runtime/` |

---

# 28. Rules for Codex before changing code

```mermaid
flowchart TB
    TASK["Receive task"]
    READ["Read AGENTS + relevant context"]
    STATUS["git status --short"]
    TRACE["Trace relevant source path"]
    PLAN["Choose smallest coherent change"]
    EDIT["Edit"]
    BUILD["Compile relevant/full target"]
    GATE["Run Step59 if lib/canonical/cross-cutting"]
    DIFF["Review git diff"]
    DONE["Done"]
    INVEST["Investigate unexpected behavior"]

    TASK --> READ --> STATUS --> TRACE --> PLAN --> EDIT --> BUILD --> GATE
    GATE -->|"expected fingerprint"| DIFF --> DONE
    GATE -->|"failure / changed fingerprint"| INVEST --> TRACE
```

---

# 29. What not to do

```mermaid
flowchart TB
    BAD["Avoid"]
    BAD --> W1["Do not make one-line main + pointless application wrapper"]
    BAD --> W2["Do not split coherent code into dozens of tiny files"]
    BAD --> W3["Do not change economic semantics during readability cleanup"]
    BAD --> W4["Do not hide known RealTest differences"]
    BAD --> W5["Do not trust historical runtime as current design"]
    BAD --> W6["Do not accept changed fingerprint without investigation"]
    BAD --> W7["Do not log secrets"]
    BAD --> W8["Do not run long/full-history jobs unnecessarily"]
```

---

# 30. Desired final architecture principle

```mermaid
flowchart TB
    CORE["One current trading core in lib/"]
    LIVE["Live services"]
    REPLAY["Canonical replay"]
    RESEARCH["Research workflows"]
    DASH["Dashboard"]
    ADAPTER["Hyperliquid / exchange adapters"]

    CORE --> LIVE
    CORE --> REPLAY
    CORE --> RESEARCH
    CORE --> DASH
    CORE --> ADAPTER
```

Different workflows may have different orchestration, reporting, adapters, or execution environments, but they should not silently duplicate core economic logic.

