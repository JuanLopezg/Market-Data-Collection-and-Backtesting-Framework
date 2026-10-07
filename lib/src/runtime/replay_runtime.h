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
#include "market_messages.h"
#include "mock/exchange.h"
#include "mock/hash.h"
#include "planning/notional_order_planner.h"
#include "portfolio_risk_engine.h"
#include "rolling_market_state.h"
#include "strategy_signal_engine.h"

// Replay orchestration and accepted component fingerprints. Keep evidence values unchanged across structural refactors.

namespace Replay {

inline constexpr const char* kStep49VenueFingerprint = "a6d2b98a45d11e57bc0d649ff9569952c73adff1671608f5eafe561169ccbb27";
inline constexpr const char* kStep50LifecycleFingerprint = "bf5e867c6793f7b9ad3c7487f6bdb2d95d296e827406a7c3870533833c09b1c3";
inline constexpr const char* kStep51MatchingFingerprint = "d06a71a4cbc7080cd4c05f7d5b3bc84ec06b55138d25590b8eefbb251e7c4720";
inline constexpr const char* kStep52AccountingFingerprint = "4fa2a7cc0e8c43f75f7fdcab2cd5b305755c48426ac3f13b4c10ed765dfac746";
inline constexpr const char* kStep53RecoveryFingerprint = "d7801a18523cf93e61f277373b2e2190070137f74ada03135fc79d1d5005dfdc";
inline constexpr const char* kStep54ReconciliationLedgerFingerprint = "8ea5f659e3bdb3f57c2e0a3408a8ab4a2fb017bdef648268bcbcf3cf9d6c0a2b";
inline constexpr const char* kStep55FaultChaosFingerprint = "7bb82df68ea1187bd59bb40c9907a0186c8829935da623a6538d2809ac6f16ba";
inline constexpr const char* kStep56RuntimeFingerprint = "79ec33ef33b9fb45410b1a10038a74dc0ac6b00f064e75175bb9330f2fddf27f";

} // namespace Replay

// Deterministically connect strategy signals, portfolio risk, order planning and
// the canonical MOCK venue for full-system historical replay.
//
// The runtime keeps replay-only orchestration in one place while reusing the same economic
// components used by the current system. Replay timestamps come from historical events,
// never from wall-clock time.

namespace Replay {

struct ReplayEvidence {
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

struct ReplayResumeState {
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
    ReplayEvidence evidence;
};

class ReplayRuntime {
public:
    ReplayRuntime(
        StrategySignalEngine& strategy_signal,
        PortfolioRiskEngine& portfolio_risk,
        NotionalOrderPlanner& order_planner,
        std::vector<StrategyID> strategy_ids,
        MockVenue::MockExchange& mock_adapter,
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

    ReplayEvidence finalizeEvidence(
        Timestamp snapshot_time);

    ReplayResumeState snapshotResumeState() const;

    void restoreResumeState(const ReplayResumeState& state);

private:
    StrategySignalEngine& strategy_signal_;
    PortfolioRiskEngine& portfolio_risk_;
    NotionalOrderPlanner& order_planner_;
    std::vector<StrategyID> strategy_ids_;

    MockVenue::MockExchange&
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

    ReplayEvidence evidence_;

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

} // namespace Replay
