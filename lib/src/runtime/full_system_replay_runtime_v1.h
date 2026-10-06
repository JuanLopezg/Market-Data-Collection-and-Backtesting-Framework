#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "account_snapshot.h"
#include "execution_order.h"
#include "full_system_replay_fingerprints_v1.h"
#include "market_slice_snapshot.h"
#include "mock_exchange_adapter_v1.h"
#include "mock_sha256_v1.h"
#include "notional_order_planner_engine.h"
#include "portfolio_risk_engine.h"
#include "rolling_market_state.h"
#include "strategy_signal_engine.h"

/**************************************************************************************
 * Type    : FullSystemReplayRuntimeV1
 * Purpose : Deterministically connect strategy signals, portfolio risk, order planning and
 *           the canonical MOCK venue for full-system historical replay.
 *
 * The runtime keeps replay-only orchestration in one place while reusing the same economic
 * components used by the current system. Replay timestamps come from historical events,
 * never from wall-clock time.
 **************************************************************************************/

namespace FullSystemReplayV1 {

struct ReplayRuntimeEvidenceV1 {
    std::uint64_t closed_slices = 0U;
    std::uint64_t execution_opens = 0U;
    std::uint64_t signal_batches = 0U;
    std::uint64_t decision_batches = 0U;
    std::uint64_t planning_batches = 0U;
    std::uint64_t planned_submits = 0U;
    std::uint64_t planned_cancels = 0U;
    std::uint64_t canonical_fills = 0U;
    std::uint64_t canonical_accounting_events = 0U;
    std::uint64_t clean_reconciliations = 0U;
    std::uint64_t blocked_or_pending_reconciliations = 0U;

    std::string step49_venue_fingerprint = kStep49VenueFingerprint;
    std::string step50_lifecycle_fingerprint = kStep50LifecycleFingerprint;
    std::string step51_matching_fingerprint = kStep51MatchingFingerprint;
    std::string step52_accounting_fingerprint = kStep52AccountingFingerprint;
    std::string step53_recovery_fingerprint = kStep53RecoveryFingerprint;
    std::string step54_reconciliation_ledger_fingerprint =
        kStep54ReconciliationLedgerFingerprint;
    std::string step55_fault_chaos_fingerprint = kStep55FaultChaosFingerprint;
    std::string step56_runtime_fingerprint = kStep56RuntimeFingerprint;

    std::string economic_fingerprint;
    std::string stream_fingerprint;
    std::string ledger_head_hash;
    std::string chaos_evidence_fingerprint;
    std::string full_run_fingerprint;
};

struct ReplayResumeStateV1 {
    RollingMarketState rolling_market;
    std::optional<StrategyIntentBatch> last_signal_batch;
    StrategyPositionSnapshot strategy_positions;
    std::vector<TrackedOrder> planning_orders;
    std::vector<FillID> processed_fill_ids;
    std::optional<NotionalOrderPlannerResult> pending_plan;

    OrderID next_order_id = 1U;
    std::uint64_t planning_revision = 0U;
    Timestamp last_closed_timestamp = 0U;
    Timestamp pending_decision_timestamp = 0U;
    Timestamp last_execution_timestamp = 0U;

    bool route_safe = true;
    double parity_cash = 0.0;
    std::vector<::Fill> parity_fills;
    ReplayRuntimeEvidenceV1 evidence;
};

class FullSystemReplayRuntimeV1 {
public:
    FullSystemReplayRuntimeV1(
        StrategySignalEngine& strategy_signal,
        PortfolioRiskEngine& portfolio_risk,
        NotionalOrderPlannerEngine& order_planner,
        std::vector<StrategyID> strategy_ids,
        MockVenueV1::MockExchangeAdapterV1& mock_adapter,
        bool preserve_decimal_price_roundtrip = false,
        bool resolve_realtest_target_at_execution_open = false);

    void onClosedSlice(
        const MarketSliceSnapshot& slice);

    void onExecutionOpen(
        Timestamp execution_timestamp,
        const ExecutionReferencePrices& open_prices);

    bool hasPendingPlan() const;

    const NotionalOrderPlannerResult&
    pendingPlan() const;

    const StrategyPositionSnapshot&
    strategyPositions() const;

    const std::vector<::Fill>& parityFills() const;

    const OrderManager&
    planningOrderManager() const;

    bool routeSafe() const;

    ReplayRuntimeEvidenceV1 finalizeEvidence(
        Timestamp snapshot_time);

    ReplayResumeStateV1 snapshotResumeState() const;

    void restoreResumeState(const ReplayResumeStateV1& state);

private:
    StrategySignalEngine& strategy_signal_;
    PortfolioRiskEngine& portfolio_risk_;
    NotionalOrderPlannerEngine& order_planner_;
    std::vector<StrategyID> strategy_ids_;

    MockVenueV1::MockExchangeAdapterV1&
        mock_adapter_;
    CanonicalVenueAdapter& venue_adapter_;

    RollingMarketState rolling_market_;
    StrategyPositionSnapshot strategy_positions_;
    OrderManager planning_order_manager_;

    std::optional<NotionalOrderPlannerResult>
        pending_plan_;
    std::optional<StrategyIntentBatch>
        last_signal_batch_;

    OrderID next_order_id_ = 1U;
    std::uint64_t planning_revision_ = 0U;
    Timestamp last_closed_timestamp_ = 0U;
    Timestamp pending_decision_timestamp_ = 0U;
    Timestamp last_execution_timestamp_ = 0U;

    bool route_safe_ = true;
    bool preserve_decimal_price_roundtrip_ = false;
    bool resolve_realtest_target_at_execution_open_ = false;

    struct ParityExpectedFill {
        double quantity = 0.0;
        double price = 0.0;
        OrderSide side = OrderSide::Buy;
    };

    double parity_cash_ = 0.0;
    std::unordered_map<OrderID, ParityExpectedFill> parity_expected_fills_;
    std::vector<::Fill> parity_fills_;

    ReplayRuntimeEvidenceV1 evidence_;

    std::unordered_map<
        std::string,
        std::unordered_map<std::string, OrderID>>
        request_items_;

    static std::uint64_t fillIdHash(
        const std::string& native_fill_id);

    static double unitsToDouble(
        std::uint64_t units,
        std::size_t scale);

    static double decimalGridUnitsToDouble(
        std::uint64_t units,
        std::size_t scale);

    std::uint64_t quantizePriceFloor(
        double value,
        std::size_t scale) const;

    std::uint64_t quantizePriceCeil(
        double value,
        std::size_t scale) const;

    double priceUnitsToDouble(
        std::uint64_t units,
        std::size_t scale) const;

    static std::uint64_t quantizeFloor(
        double value,
        std::size_t scale);

    static std::uint64_t quantizeCeil(
        double value,
        std::size_t scale);

    ::AccountSnapshot portfolioRiskAccountView(
        Timestamp timestamp) const;

    void markCurrentPositionsAtClose(
        Timestamp timestamp,
        const std::vector<MarketBarSnapshot>& bars);

    void routeCancelBatch(
        const NotionalOrderPlannerResult& plan,
        Timestamp execution_timestamp);

    void routeSubmitBatch(
        const NotionalOrderPlannerResult& plan,
        Timestamp execution_timestamp,
        const ExecutionReferencePrices&
            open_prices);

    double realtestParityOrderQuantity(
        const PlannedNotionalOrder& planned,
        const ExecutionReferencePrices& open_prices) const;

    VenueContracts::V1::LimitOrderIntent
    canonicalOrder(
        const PlannedNotionalOrder& planned,
        Timestamp execution_timestamp,
        const ExecutionReferencePrices&
            open_prices) const;

    VenueContracts::V1::RequestIdentity
    makeRequest(
        const std::string& id,
        Timestamp timestamp) const;

    void rememberRequestItems(
        const VenueContracts::V1::
            SubmitOrderBatch& batch);

    void rememberRequestItems(
        const VenueContracts::V1::
            CancelOrderBatch& batch);

    void rejectPlannedSubmitsLocally(
        const NotionalOrderPlannerResult& plan,
        Timestamp timestamp,
        const std::string& reason);

    void onVenueEvent(
        const VenueContracts::V1::Event& event);

    void onOperationResult(
        const VenueContracts::V1::
            OperationResult& result);

    void markMirrorRejected(
        OrderID order_id,
        Timestamp timestamp,
        const std::string& message);

    void onOrderUpdate(
        const VenueContracts::V1::
            OrderUpdate& update);

    void onFill(
        const VenueContracts::V1::Fill& fill);
};

} // namespace FullSystemReplayV1
