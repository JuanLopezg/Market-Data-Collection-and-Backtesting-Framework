// Reconciler quantity tolerance: rounding noise on very large positions (cheap coins held
// in millions of units) is not drift, while a real difference of one unit still blocks.
#include <cassert>
#include <cmath>
#include <iostream>

#include "reconciler.h"

namespace {

ReconciliationReport comparePosition(double local, double exchange)
{
    TradingStateSnapshot state;
    state.account_cash = 1000.0;
    state.account_positions["1000PEPEUSDT"] = local;
    ExchangeSnapshot venue;
    venue.timestamp = 20261014;
    venue.cash = 1000.0;
    venue.positions["1000PEPEUSDT"] = exchange;
    return Reconciler().compare(state, venue);
}

} // namespace

int main()
{
    // 711,394 units (tonight's PAPER PEPE order): three ULPs of rounding noise are ~3.5e-10
    // units, more than the old fixed 1e-10 tolerance, which would have paused trading.
    const double venue = 711394.39137621;
    double noisy = venue;
    for (int ulp = 0; ulp < 3; ++ulp)
        noisy = std::nextafter(noisy, 1e300);
    assert(std::abs(noisy - venue) > 1e-10);
    assert(comparePosition(noisy, venue).clean());

    // Ten thousand million units: noise of a few ULPs (~2e-6) is accepted ...
    assert(comparePosition(1e10, 1e10 + 4e-6).clean());
    // ... but one unit missing on the venue is real drift and still pauses trading.
    const ReconciliationReport drift = comparePosition(1e10, 1e10 - 1.0);
    assert(!drift.clean() && drift.issues.size() == 1);
    assert(drift.issues[0].kind == ReconciliationIssueKind::PositionMismatch);

    // Small positions keep the absolute tolerance: 0.001 BTC difference is drift.
    TradingStateSnapshot state;
    state.account_positions["BTCUSDT"] = 0.5;
    ExchangeSnapshot exchange;
    exchange.timestamp = 20261014;
    exchange.positions["BTCUSDT"] = 0.499;
    assert(!Reconciler().compare(state, exchange).clean());
    exchange.positions["BTCUSDT"] = 0.5 + 5e-11;
    assert(Reconciler().compare(state, exchange).clean());

    // A position only one side knows about is always drift, however large.
    exchange.positions.erase("BTCUSDT");
    assert(!Reconciler().compare(state, exchange).clean());

    std::cout << "reconciler quantity tolerance: PASS\n";
    return 0;
}
