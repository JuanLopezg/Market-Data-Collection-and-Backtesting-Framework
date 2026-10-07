#include <cassert>
#include <cmath>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "manual_trading.h"

using namespace ManualControl;
using namespace MockVenue;

namespace {
ExecutionReferencePrices prices(double p)
{
    ExecutionReferencePrices x;
    x.set("BTCUSDT", p);
    return x;
}

MarketBarObservation bar(Timestamp ts, double open, double high, double low, double close, double volume)
{
    MarketBarObservation b;
    b.canonical_asset = "BTCUSDT";
    b.event_time = ts;
    b.bar.open = open;
    b.bar.high = high;
    b.bar.low = low;
    b.bar.close = close;
    b.bar.volume = volume;
    assert(b.valid());
    return b;
}

ManualTargetIntent intent(const std::string& id, Timestamp decision, Timestamp execution, double btc_weight)
{
    ManualTargetIntent i;
    i.request_id = id;
    i.correlation_id = "corr-" + id;
    i.actor = "operator / OPERATOR";
    i.request_hash = "sha256:" + id;
    i.decision_timestamp = decision;
    i.execution_timestamp = execution;
    if (btc_weight > 0.0)
        i.asset_weights.emplace("BTCUSDT", btc_weight);
    i.cash_weight = 1.0 - btc_weight;
    assert(i.valid());
    return i;
}
}

int main()
{
    const auto dir = std::filesystem::temp_directory_path() /
        ("algoTrading-step57-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    MockChaosConfig chaos;
    chaos.submit_limit = 100U;
    chaos.cancel_limit = 100U;
    chaos.reconcile_limit = 100U;
    chaos.market_source_limit = 1000U;

    MockExchange adapter(dir, chaos);
    ManualTrading pipeline(adapter);

    // 100% cash while already flat is a truthful NOOP.
    const auto initial_noop = pipeline.route(intent("initial-noop", 90, 95, 0.0), prices(100.0), prices(100.0));
    assert(initial_noop.status == ManualRouteStatus::Noop);
    assert(!initial_noop.command_dispatched);

    // 50% BTC / 50% cash: risk -> production planner -> canonical adapter -> MOCK.
    const auto entry = pipeline.route(intent("entry", 100, 110, 0.50), prices(100.0), prices(101.0));
    assert(entry.status == ManualRouteStatus::Submitted);
    assert(entry.command_dispatched);
    assert(entry.reconciliation_clean);
    assert(entry.submit_count == 1U);
    assert(adapter.chaos().runtime().lifecycle().orders().size() == 1U);

    const auto fill_entry = pipeline.processMarketBar(bar(120, 101.0, 102.0, 99.0, 101.0, 100000.0));
    assert(fill_entry.status == ChaosStatus::Delivered);
    const double quantity = pipeline.strategyPositions().at(kManualStrategyId).get("BTCUSDT");
    assert(quantity > 0.0);
    assert(adapter.chaos().runtime().account().accountSnapshot(120).positions.size() == 1U);

    // 100% cash creates exact FLAT via the same planner/adapter path.
    const auto exit = pipeline.route(intent("exit", 150, 160, 0.0), prices(101.0), prices(100.0));
    assert(exit.status == ManualRouteStatus::Submitted);
    assert(exit.submit_count == 1U);

    const auto fill_exit = pipeline.processMarketBar(bar(170, 100.0, 101.0, 98.0, 100.0, 100000.0));
    assert(fill_exit.status == ChaosStatus::Delivered);
    assert(std::abs(pipeline.strategyPositions().at(kManualStrategyId).get("BTCUSDT")) < 1e-12);
    assert(adapter.chaos().runtime().account().accountSnapshot(170).positions.empty());

    // Final venue truth reconciles cleanly; ledger contains both fills + fees.
    const auto recon = adapter.reconcile(170);
    assert(recon.clean());
    MockReconciliation reconciler;
    const auto local = reconciler.buildLocalExpected(adapter.chaos().runtime().stream());
    assert(local.complete);
    assert(local.ledger.valid);
    assert(local.ledger.fills.size() == 2U);
    assert(local.ledger.entries.size() == 4U);

    // Risk rejects unmapped assets before planner/venue.
    ManualTargetIntent bad = intent("bad", 180, 190, 0.0);
    bad.asset_weights.emplace("NOTREGISTERED", 0.5);
    bad.cash_weight = 0.5;
    const auto rejected = pipeline.route(bad, prices(100.0), prices(100.0));
    assert(rejected.status == ManualRouteStatus::RiskRejected);
    assert(!rejected.command_dispatched);

    std::filesystem::remove_all(dir, ignored);
    return 0;
}
