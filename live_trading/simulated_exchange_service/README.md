# Simulated exchange position stops

`ExecutionOrder::position_stop_price` is a resting reduce-only SELL. Execution supplies
the price and quantity; the simulator does not calculate the 70% daily level or manage
replacement policy. Two stops on the same coin may coexist until execution cancels the
old one after the new Accepted event.

`SimulatedExchange::processPositionStopsAtClose(day, bars, account.positions())` consumes
completed daily bars. A stop triggers at `low <= position_stop_price`, fills at
`min(open, position_stop_price)` and sells at most the current positive position.
Triggered stops on a coin run by descending stop price, then ascending order id for
ties. Reserved units are deducted within the matching batch before the caller applies
the emitted fills, preventing overlapping stops from opening a short. A positive capped
fill ends Filled; no long position ends Canceled with a clear message. Commission and
split-fill handling use the existing fill path. Fills precede terminal order updates.
Missing bars and non-triggering bars leave a stop resting. Markets, entry stops and
research short covers retain their separate existing matching paths.

## Service bar input and recovery

The service accepts complete `market.slice.snapshot.v1` messages. With
`--market-data-db /path/to/canonical/database.db`, it also consumes
`market.data.updated.v1` and reads committed OHLCV from that SQLite database read-only.
The read catches up from the last processed bar, or the earliest active stop on a new
journal, through the completed frontier. Missing required rows retry without inventing
lows. Closed bars are handled before execution opens; with the database configured an
open waits while an eligible stop still needs the preceding completed bar.

The existing `execution.prices.v1` contract carries opens only. These are not used as
daily lows and cannot fill a resting stop as a market sell. **PAPER integration needs
the read-only market database mount and the new option**, unless an upstream producer
publishes complete slice snapshots. The chain currently publishes market updates and
execution opens, not complete slices. Deployment configuration is owned by task D and
is deliberately unchanged here.

PostgreSQL adds `simulated_exchange_known_orders.position_stop_price` (default 0 for old
market orders) and `simulated_exchange_state.last_position_stop_bar` (default 0). Stops
restore through the existing active-order/known-order relationship. Duplicate submits
compare the stop price too; changed terms under the same id are refused. The processed
bar cursor, account, orders, next FillID and outbox commit together. Redelivery of a
completed bar cannot trigger a stop submitted after that close, including on restart.
As with the gateway, an older binary that cannot distinguish stop rows must not be used
to submit prepared/restored stops; release compatibility belongs to the release owner.

## Tests

Add this exact entry to the `TESTS` array of `validation/mock_component_tests.sh`:

```bash
"position_stop_simulated_exchange_test.cpp c++20"
```

The test uses only the runner's existing include paths and library-source objects.
For an additional isolated service check after building in WSL:

```bash
python3 live_trading/simulated_exchange_service/position_stop_service_test.py
```

It requires cached local `postgres:16-alpine` and `nats:2.10-alpine` images. It runs
temporary local containers with loopback-published ports, a synthetic SQLite fixture,
and a real service process; it checks old-schema migration, restart, duplicate/conflicting
commands, OHLCV and snapshot inputs, capped accounting and the durable bar cursor.
It removes its containers and temporary state, and never accesses a trading venue.
