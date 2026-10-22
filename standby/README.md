# Local standby of the deployed chain

If the VPS is down, `standby/standby.py plan` computes on this machine the orders the chain
would send at the next open (00:00 UTC): same strategy and portfolio configuration as the
deployed release, the same lib components as the services, the account's real holdings.
**Phase 1: it never sends anything.** Sending orders from here (phase 2) is planned for
when the chain trades Kraken LIVE; see docs/ROADMAP.md, Next steps 5.

## How it works

Stateless: nothing from the VPS is needed at plan time except the profile copied by `sync`.

1. `sync` (after every release or config change on the VPS; read-only SSH) copies
   `MODE`, `STRATEGY_FILE`, `PORTFOLIO_FILE` and `LIVE_COMMISSION_RATE` from
   `chain/chain.env` (only those four lines are read), the two config files, the Binance ->
   Kraken symbol map and the current release id into `standby/profile/` (ignored by Git).
2. `plan` downloads the last `--days` (130) daily klines of every Binance USDT perpetual
   with status TRADING and writes them in the market-data service's SQLite schema
   (`ohlcv_data`, `market_volume_rank_daily`, top 100 by the day's quote volume, as the
   service ranks them right after 00:00 UTC).
3. `build/standby/algotrading_standby_plan` (standby/src/) runs the profile as the chain
   splits it, with the components of the canonical replay:
   - strategy: `StrategySignalEngine` over the last 30 completed days and T, each with the
     strategy service's window (top 50 plus active signals, 100 days). Signals do not
     depend on the account, so these days settle the signal state from a cold start;
   - portfolio risk at close(T): `PortfolioRiskEngine` with the real account and the risk
     service's window (signalled and held symbols, lookback + 5 days), including the 20%
     relative resize rule when the portfolio file uses it;
   - planning: `NotionalOrderPlanner` priced at the exact close(T), as execution-state asks
     the planner. The orders are USD amounts; the chain resolves quantities at the next
     open, so the quantities shown are estimates at close(T).
4. Each order gets its Kraken symbol and contracts (`venue_units_per_source_unit`); an
   asset without a Kraken contract is `SKIPPED: not on Kraken`, as the gateway skips it.
   One public Kraken instruments request supplies `venue_step` from
   `contractValueTradePrecision`. `venue_quantity` rounds the estimated contracts to the
   nearest step, with the gateway's half-up rule and tolerance. A zero result is
   `BELOW_MINIMUM_QUANTITY`. The original USD amounts and quantity estimates are kept.
   If metadata is unavailable, both venue fields are `null` with status
   `VENUE_STEP_UNAVAILABLE`; the plan is still written. The table shows these fields too.

The account is read like the gateway reads it (live_venue_adapter.cpp): cash = USD balance
minus the entry value of open positions (execution-state's spot convention), positions in
Binance source units, collateral = the other currencies' value after Kraken's haircut.

## If the VPS is down

Nothing here sends orders (phase 2 is not built). The steps keep a VPS outage from becoming
a second problem when it comes back.

1. **Check it really is down:** `python3 standby/standby.py check` (from WSL with the
   Windows key: `--ssh-command /mnt/c/Windows/System32/OpenSSH/ssh.exe --ssh-key
   'C:\Users\Juan\.ssh\algotrading-spain.pem'`). ALIVE means the VPS trades by itself:
   never place its orders elsewhere. UNREACHABLE: look at the Lightsail console; a reboot
   from there usually brings the chain back (the units start it) and nothing else is needed.
2. **While it is down, the account is still protected:** the 30% stop-loss orders rest at
   Kraken and work without the VPS. Missing one daily cycle only delays entries/exits.
3. **See what it would have done:** `python3 standby/standby.py plan` (yesterday's close, the
   Kraken account through the read-only key). It prints each order with its Kraken symbol and
   the venue quantity rounded like the gateway. The profile is the one of the last `sync`,
   so run `sync` after every release while the VPS is alive.
4. **Placing those orders by hand in Kraken is possible but has a cost:** when the VPS comes
   back, its state does not know them, so the next reconciliation finds different positions
   and pauses trading. Before starting the chain again, either undo the manual orders (back
   to the positions the VPS last knew, `algo ps`/Positions page before the outage), or ask
   for help to adopt them. Positions opened by hand have no stop-loss until the VPS sets one.
5. **When it is back:** `standby.py check` must say ALIVE with a clean reconciliation; the
   market data catches up the missed day by itself.

## Commands (WSL, repository root)

```bash
meson compile -C build algotrading_standby_plan
python3 standby/standby.py check           # is the VPS alive? (read-only)
python3 standby/standby.py sync            # needs SSH to the VPS (also works from Windows)
python3 standby/standby.py plan            # yesterday's close, Kraken account (read-only key)
python3 standby/standby.py plan --date 2026-10-14 --account-file account.json
```

`--credentials` points at the read-only Kraken key file (default
`storage/kraken_shadow/.env`, read only by the program). `--account-file` takes
`{"cash": ..., "collateral_usd": ..., "positions": {"BTCUSDT": 0.1}}` instead (validation,
or a PAPER account). Outputs go to `standby/run/<decision day>/` (ignored): `market.sqlite`,
`manifest.json`, `plan-raw.json` and `plan.json` (`"sent": false`). `--reuse-data` reruns a
day without downloading again.

## Validation against the VPS (2026-10-15)

With the PAPER account the VPS had before each decision (comparison manifest, read-only),
the plans for closes 20261013, 20261014 and 20261015 equal the VPS decisions exactly:
same sizing capital (92,230.58 / 91,699.43 / 91,992.67 USD), same signals, same actions and
target weights, and the same order amounts to the cent (for example 20261014: sell AVAX
2,926.81 and ONDO 2,842.87 USD).

## Limits

- The ranking uses each day's kline quote volume and today's TRADING list; the VPS used the
  24 h tickers at 00:00 of each day. A contract that changed status in the window may rank
  differently on old days; the decision day itself is the same data.
- The local build must be the deployed source. `sync` records the release id
  (`<stamp>-<commit>[-dirty]`); build the standby from that commit.
- Venue quantities use close(T) estimates and current public lot steps; the gateway
  recalculates quantities at the open, so the rounded quantity and minimum-size status
  can change. This plan does not check venue admission or confirm execution.
