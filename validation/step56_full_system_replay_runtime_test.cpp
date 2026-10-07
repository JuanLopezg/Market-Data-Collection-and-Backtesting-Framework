#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "replay_runtime.h"
#include "risk_constraints.h"
#include "strategy.h"

using namespace Replay;
using namespace MockVenue;

namespace {

class ThresholdStrategy final : public Strategy {
public:
    ThresholdStrategy()
        : Strategy(
            "Threshold_Test",
            1U,
            nullptr,
            nullptr,
            1U)
    {
    }

    void updateSignals(
        const MarketData& market_data,
        Timestamp ts,
        SignalState& signals,
        const IndicatorEngine& indicators) const override
    {
        (void)indicators;

        const auto ts_it =
            market_data.find(ts);
        assert(ts_it != market_data.end());

        const auto coin_it =
            ts_it->second.find("BTCUSDT");
        assert(coin_it != ts_it->second.end());

        if (coin_it->second.close >= 101.0)
            signals.set("BTCUSDT", 1.0);
        else if (coin_it->second.close <= 99.0)
            signals.set("BTCUSDT", 0.0);
    }

    std::vector<IndicatorSpec>
    requiredIndicators() const override
    {
        return {};
    }
};

MarketSliceSnapshot slice(
    Timestamp ts,
    double open,
    double high,
    double low,
    double close,
    double volume)
{
    MarketSliceSnapshot s;
    s.timestamp = ts;

    MarketBarSnapshot bar;
    bar.coin = "BTCUSDT";
    bar.bar.open = open;
    bar.bar.high = high;
    bar.bar.low = low;
    bar.bar.close = close;
    bar.bar.volume = volume;

    s.bars.push_back(bar);
    return s;
}

ExecutionReferencePrices openPrice(
    double value)
{
    ExecutionReferencePrices prices;
    prices.set("BTCUSDT", value);
    return prices;
}

} // namespace

int main()
{
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("algoTrading-step56-" +
         std::to_string(::getpid()));

    std::error_code ignored;
    std::filesystem::remove_all(
        dir,
        ignored);

    StrategySignalPortfolio strategies;
    strategies.emplace_back(
        1U,
        std::make_unique<ThresholdStrategy>());

    StrategySignalEngine strategy_engine(
        std::move(strategies));

    std::vector<PortfolioRiskStrategyConfig>
        risk_configs;

    risk_configs.emplace_back(
        1U,
        "Threshold_Test",
        1.0,
        std::make_unique<EqualWeightSizer>(
            0.10),
        RiskConstraints(
            1.0,
            0.10),
        std::make_unique<
            EntryExitOnlyRebalancePolicy>());

    PortfolioRiskEngine risk_engine(
        std::move(risk_configs));

    NotionalOrderPlanner planner;

    MockChaosConfig chaos;
    chaos.submit_limit = 100U;
    chaos.cancel_limit = 100U;
    chaos.modify_limit = 100U;
    chaos.market_source_limit = 1000U;
    chaos.reconcile_limit = 100U;
    chaos.stream_read_limit = 1000U;

    MockExchange adapter(
        dir,
        chaos);

    // Interface contract is now concretely implemented.
    CanonicalVenueAdapter& canonical =
        adapter;
    assert(
        canonical.context().venue_id ==
        "MOCK");
    assert(
        canonical.capabilities().supportsAll({
            VenueContracts::V1::Capability::SubmitOrder,
            VenueContracts::V1::Capability::AccountSnapshot,
            VenueContracts::V1::Capability::Fills,
            VenueContracts::V1::Capability::UserStream
        }));

    ReplayRuntime replay(
        strategy_engine,
        risk_engine,
        planner,
        {1U},
        adapter);

    // T=100: no signal.
    replay.onClosedSlice(
        slice(
            100,
            100.0,
            101.0,
            99.0,
            100.0,
            5000.0));

    assert(replay.hasPendingPlan());
    assert(
        replay.pendingPlan().
            submit_orders.empty());

    replay.onExecutionOpen(
        150,
        openPrice(100.0));

    // T=200 close triggers strategy entry.
    replay.onClosedSlice(
        slice(
            200,
            100.0,
            103.0,
            99.0,
            102.0,
            5000.0));

    assert(replay.hasPendingPlan());
    assert(
        replay.pendingPlan().
            submit_orders.size() == 1U);
    assert(
        replay.pendingPlan().
            submit_orders[0].
            decision_timestamp == 200U);
    assert(
        replay.pendingPlan().
            submit_orders[0].
            coin == "BTCUSDT");

    // Admission occurs at later open, through CanonicalVenueAdapter.
    replay.onExecutionOpen(
        250,
        openPrice(103.0));

    const StoredOrder* admitted =
        adapter.chaos().runtime().
            lifecycle().findOrder(1U);

    assert(admitted != nullptr);
    assert(
        admitted->intent.active_from ==
        250U);
    assert(
        admitted->intent.limit_price ==
        103.0);

    // T=300 completed bar fills the already-admitted order before Strategy sees T=300.
    replay.onClosedSlice(
        slice(
            300,
            103.0,
            105.0,
            102.0,
            104.0,
            5000.0));

    const auto positions_after_entry =
        adapter.chaos().runtime().
            account().accountSnapshot(300);

    assert(
        positions_after_entry.
            positions.size() == 1U);
    assert(
        positions_after_entry.
            positions[0].
            signed_quantity > 0.0);

    const auto strategy_position =
        replay.strategyPositions().
            at(1U).get("BTCUSDT");

    assert(strategy_position > 0.0);
    assert(
        replay.planningOrderManager().
            find(1U) != nullptr);
    assert(
        replay.planningOrderManager().
            find(1U)->status ==
        ExecutionOrderStatus::Filled);

    // Same-side signal at T=300 produces no resize under EntryExitOnly.
    assert(replay.hasPendingPlan());
    assert(
        replay.pendingPlan().
            submit_orders.empty());

    replay.onExecutionOpen(
        350,
        openPrice(104.0));

    // T=400 close exits signal -> planner creates FLAT sell.
    replay.onClosedSlice(
        slice(
            400,
            100.0,
            101.0,
            97.0,
            98.0,
            5000.0));

    assert(replay.hasPendingPlan());
    assert(
        replay.pendingPlan().
            submit_orders.size() == 1U);
    assert(
        replay.pendingPlan().
            submit_orders[0].side ==
        OrderSide::Sell);

    replay.onExecutionOpen(
        450,
        openPrice(97.0));

    const StoredOrder* exit_order =
        adapter.chaos().runtime().
            lifecycle().findOrder(2U);

    assert(exit_order != nullptr);
    assert(
        exit_order->intent.active_from ==
        450U);

    replay.onClosedSlice(
        slice(
            500,
            97.0,
            99.0,
            96.0,
            97.0,
            5000.0));

    const auto final_account =
        adapter.chaos().runtime().
            account().accountSnapshot(500);

    assert(final_account.positions.empty());
    assert(
        std::abs(
            replay.strategyPositions().
                at(1U).get("BTCUSDT"))
        < 1e-12);

    const auto evidence =
        replay.finalizeEvidence(500);

    assert(evidence.closed_slices == 5U);
    assert(evidence.execution_opens == 4U);
    assert(evidence.signal_batches == 5U);
    assert(evidence.decision_batches == 5U);
    assert(evidence.planning_batches == 5U);
    assert(evidence.planned_submits == 2U);
    assert(evidence.canonical_fills == 2U);
    assert(
        evidence.canonical_accounting_events ==
        2U);
    assert(
        evidence.economic_fingerprint.size() ==
        16U);
    assert(
        evidence.stream_fingerprint.size() ==
        16U);
    assert(
        evidence.ledger_head_hash.size() ==
        64U);
    assert(
        evidence.chaos_evidence_fingerprint.size() ==
        64U);
    assert(
        evidence.full_run_fingerprint.size() ==
        64U);

    assert(
        evidence.step49_venue_fingerprint ==
        kStep49VenueFingerprint);
    assert(
        evidence.step55_fault_chaos_fingerprint ==
        kStep55FaultChaosFingerprint);
    assert(
        evidence.step56_runtime_fingerprint ==
        kStep56RuntimeFingerprint);

    const auto final_recon =
        adapter.reconcile(500);
    assert(final_recon.clean());
    assert(replay.routeSafe());

    // Adapter query surfaces emit canonical read events without bypassing MOCK state.
    std::vector<VenueContracts::V1::Event>
        query_events;

    canonical.setEventHandler(
        [&](const VenueContracts::V1::Event& e) {
            query_events.push_back(e);
        });

    VenueContracts::V1::AccountSnapshotRequest
        account_request;
    account_request.request.venue =
        canonical.context();
    account_request.request.request_id =
        "query-account";
    account_request.request.requested_at =
        500U;

    canonical.requestAccountSnapshot(
        account_request);

    assert(query_events.size() == 1U);
    assert(
        std::holds_alternative<
            VenueContracts::V1::AccountSnapshot>(
                query_events[0]));

    std::filesystem::remove_all(
        dir,
        ignored);

    return 0;
}
