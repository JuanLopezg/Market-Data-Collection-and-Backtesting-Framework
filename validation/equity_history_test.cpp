// Daily equity rows of ExecutionState (live_trading/execution_state_service/src/equity_history.*).
//
// The case behind the 2026-10-15 fix: a day's row is the account at close(T). A fill of
// T+1's open must not change it, which the rebuild from the fills guarantees and which the
// live path (a market update redelivered after a restart) now guarantees by writing a
// day's row only once (PostgresStateStore::insertEquityDay, tested against PostgreSQL in
// transport_persistence_integration_test).
#include <cassert>
#include <cmath>
#include <iostream>

#include "equity_history.h"

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

Fill fill(FillID id, Timestamp day, OrderSide side, double quantity, double price, double fee)
{
    Fill value;
    value.fill_id = id;
    value.order_id = id;
    value.strategy_id = 1;
    value.timestamp = day;
    value.coin = "BTCUSDT";
    value.side = side;
    value.quantity = quantity;
    value.price = price;
    value.commission = fee;
    return value;
}

VenueCostEvent movement(VenueCostKind kind, Timestamp day, double amount)
{
    VenueCostEvent value;
    value.kind = kind;
    value.timestamp = day;
    value.amount_usd = amount;
    return value;
}

// BTCUSDT closes: 105 on the 13th, 120 on the 14th; nothing for the 15th.
std::optional<std::unordered_map<Coin, double>> closes(Timestamp day, const std::set<Coin>& coins)
{
    const std::unordered_map<Timestamp, double> btc{{20261012, 99.0}, {20261013, 105.0}, {20261014, 120.0}};
    if (!btc.contains(day))
        return std::nullopt;
    std::unordered_map<Coin, double> result;
    for (const Coin& coin : coins)
        result[coin] = btc.at(day);
    return result;
}

double noCollateral(Timestamp) { return 0.0; }

} // namespace

int main()
{
    // Bought at the open of the 13th, sold at the open of the 14th (as PAPER sold AVAX/ONDO
    // at the open of 2026-10-15 after the row of 2026-10-14 was written).
    const std::vector<Fill> fills{fill(1, 20261013, OrderSide::Buy, 2, 100, 1),
                                  fill(2, 20261014, OrderSide::Sell, 2, 110, 1)};

    const std::vector<EquityDay> rows = equityDaysFromFills(fills, {}, {}, 1000.0, 20261014, {}, closes, noCollateral);
    assert(rows.size() == 2);
    // 13th: cash 1000 - 200 - 1 = 799, 2 x 105 = 210, equity 1009; the next open's sale is not in it.
    assert(rows[0].date == 20261013 && near(rows[0].cash, 799) && near(rows[0].positions_value, 210) &&
           near(rows[0].equity, 1009) && rows[0].open_positions == 1 && near(rows[0].fees, 1));
    // 14th: sold at 110 (fee 1): cash 799 + 220 - 1 = 1018, flat.
    assert(rows[1].date == 20261014 && near(rows[1].cash, 1018) && near(rows[1].positions_value, 0) &&
           near(rows[1].equity, 1018) && rows[1].open_positions == 0);

    // What the old live path wrote after a restart: the 13th marked with the account that
    // already held the sale of the 14th. Different from the true row, hence write-once.
    Account afterNextOpen(1000.0);
    PositionLedger ledgerAfterNextOpen;
    for (const Fill& value : fills) {
        afterNextOpen.applyFill(value);
        ledgerAfterNextOpen.applyFill(value);
    }
    const EquityDay overwritten = markEquityDay(20261013, afterNextOpen.cash(), afterNextOpen.positions().values(),
                                                *closes(20261013, {}), ledgerAfterNextOpen, {}, 0.0);
    assert(near(overwritten.equity, 1018) && !near(overwritten.equity, rows[0].equity));

    // Existing rows are kept (only missing days are rebuilt); a day without closes is left out.
    const std::vector<EquityDay> missingOnly = equityDaysFromFills(fills, {}, {}, 1000.0, 20261015, {20261013},
                                                                   closes, noCollateral);
    assert(missingOnly.size() == 1 && missingOnly[0].date == 20261014);

    // Funding and account movements up to each day; capital flows/interest are recorded
    // beside the equity (flows are in cash, not performance).
    const std::vector<FundingPayment> funding{FundingPayment{"f1", 20261014, "BTCUSDT", -0.5}};
    const std::vector<VenueCostEvent> movements{movement(VenueCostKind::CapitalFlow, 20261013, 50.0),
                                                movement(VenueCostKind::Interest, 20261014, -0.25)};
    const std::vector<EquityDay> withCosts = equityDaysFromFills(
        fills, funding, movements, 1000.0, 20261014, {}, closes, [](Timestamp day) { return day == 20261014 ? 7.0 : 0.0; });
    assert(withCosts.size() == 2);
    assert(near(withCosts[0].cash, 849) && near(withCosts[0].capital_flows, 50) && near(withCosts[0].interest, 0));
    assert(near(withCosts[1].cash, 849 + 220 - 1 - 0.5 - 0.25) && near(withCosts[1].funding, -0.5) &&
           near(withCosts[1].interest, -0.25) && near(withCosts[1].collateral_usd, 7));

    // Replay timestamps carry a phase digit: the fill of 202610131 belongs to the 13th.
    assert(tradingDayOf(202610131) == 20261013 && tradingDayOf(20261013) == 20261013);
    const std::vector<EquityDay> replay = equityDaysFromFills({fill(1, 202610131, OrderSide::Buy, 2, 100, 1)}, {}, {},
                                                              1000.0, 20261013, {}, closes, noCollateral);
    assert(replay.size() == 1 && near(replay[0].equity, 1009));

    // The history is rebuilt only when the fills lead to the live account.
    assert(fillsReachAccount(fills, {}, {}, 1000.0, afterNextOpen));
    Account other(1000.0);
    assert(!fillsReachAccount(fills, {}, {}, 1000.0, other));
    assert(equityDaysFromFills({}, {}, {}, 1000.0, 20261014, {}, closes, noCollateral).empty());

    std::cout << "equity_history_test: PASS\n";
    return 0;
}
