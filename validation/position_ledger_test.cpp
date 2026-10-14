// PositionLedger: the same scenario as the dashboard's former Go rebuild
// (TestPositionHistoryAveragesReducesAndFlips), so moving the computation into the
// runtime does not change any reported number. Plus idempotency and rebuild order.
#include <cassert>
#include <cmath>
#include <iostream>

#include "position_ledger.h"

namespace {

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

Fill fill(FillID id, Timestamp time, OrderSide side, double quantity, double price, double fee)
{
    Fill value;
    value.fill_id = id;
    value.order_id = id;
    value.strategy_id = 1;
    value.timestamp = time;
    value.coin = "BTC";
    value.side = side;
    value.quantity = quantity;
    value.price = price;
    value.commission = fee;
    return value;
}

FundingPayment funding(const char* id, Timestamp time, double amount)
{
    return FundingPayment{id, time, "BTC", amount};
}

} // namespace

int main()
{
    const std::vector<Fill> fills{
        fill(1, 202001011, OrderSide::Buy, 1, 100, 1),
        fill(2, 202001021, OrderSide::Buy, 1, 200, 2),       // average entry 150
        fill(3, 202001041, OrderSide::Sell, 0.5, 210, 0.5),  // realizes +30
        fill(4, 202001051, OrderSide::Sell, 2, 120, 4),      // closes 1.5 at 120 (-45), opens a 0.5 short
    };
    const std::vector<FundingPayment> payments{funding("f1", 202001031, -0.5), funding("f2", 202001061, 0.25)};

    // Fills arrive out of order on purpose: rebuild sorts by (timestamp, fill id).
    const PositionLedger ledger = PositionLedger::rebuild({fills[3], fills[1], fills[0], fills[2]}, payments);
    assert(ledger.closedPositions().size() == 1);
    const PositionLot& closed = ledger.closedPositions()[0];
    // Fees: 1 + 2 + 0.5 + 4 x 1.5/2 = 6.5. Gross: 30 - 45 = -15. Net: -15 - 6.5 - 0.5 = -22.
    assert(closed.long_side && near(closed.entry_price, 150) && near(closed.gross_pnl, -15));
    assert(near(closed.fees, 6.5) && near(closed.funding, -0.5) && near(closed.netPnl(), -22));
    assert(near(closed.max_quantity, 2) && closed.closed_at == 202001051 && closed.closing_fill_id == 4);
    assert(near(closed.entry_value, 300) && near(closed.exit_value, 0.5 * 210 + 1.5 * 120));
    assert(near(closed.exitPrice(), 285.0 / 2));

    const PositionLot& open = ledger.openPositions().at("BTC");
    assert(!open.long_side && near(open.quantity, -0.5) && near(open.fees, 1) && near(open.funding, 0.25));
    assert(open.opened_at == 202001051 && open.closed_at == 0);

    // Totals cover closed and open positions.
    assert(near(ledger.realizedGrossPnl(), -15) && near(ledger.fees(), 7.5) && near(ledger.funding(), -0.25));

    // Live path: applying fills one by one gives the same result, and a repeated fill or
    // funding id changes nothing.
    PositionLedger live;
    live.applyFill(fills[0]);
    live.applyFill(fills[1]);
    live.applyFunding(payments[0]);
    live.applyFunding(payments[0]);
    live.applyFill(fills[2]);
    live.applyFill(fills[2]);
    const auto closedNow = live.applyFill(fills[3]);
    assert(closedNow && closedNow->closing_fill_id == 4 && near(closedNow->netPnl(), -22));
    assert(!live.applyFill(fills[3]));
    assert(near(live.fees(), 7.5) && near(live.funding(), -0.5));

    // Funding with no open position on the asset has nothing to attach to.
    PositionLedger flat;
    flat.applyFunding(funding("f3", 20240101, 5));
    assert(near(flat.funding(), 0) && flat.openPositions().empty());

    std::cout << "position ledger: PASS\n";
    return 0;
}
