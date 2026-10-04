#pragma once

#include <algorithm>
#include <cmath>
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

#include "execution_order.h"
#include "manual_portfolio_risk_engine_v1.h"
#include "mock_exchange_adapter_v1.h"
#include "notional_order_planner_engine.h"
#include "order_manager.h"

namespace ManualControlV1 {

enum class ManualRouteStatusV1 {
    Noop = 0,
    Submitted,
    RiskRejected,
    ReconciliationBlocked,
    VenueRejected,
    AmbiguousOrPending
};

struct ManualRouteResultV1 {
    ManualRouteStatusV1 status = ManualRouteStatusV1::RiskRejected;
    std::string request_id;
    std::string correlation_id;
    std::string request_hash;
    std::string reason;
    bool command_dispatched = false;
    bool reconciliation_clean = false;
    std::size_t cancel_count = 0U;
    std::size_t submit_count = 0U;
};

class ManualControlMockPipelineV1 {
public:
    explicit ManualControlMockPipelineV1(
        MockVenueV1::MockExchangeAdapterV1& adapter,
        double max_gross_leverage = 1.0,
        double max_asset_weight = 1.0)
        : adapter_(adapter),
          venue_(adapter),
          risk_(max_gross_leverage, max_asset_weight)
    {
        strategy_positions_.emplace(kManualStrategyId, VirtualPositionState{});
        adapter_.setEventHandler([this](const VenueContracts::V1::Event& event) {
            onVenueEvent(event);
        });
    }

    ManualRouteResultV1 route(
        const ManualTargetIntentV1& intent,
        const ExecutionReferencePrices& closes,
        const ExecutionReferencePrices& opens)
    {
        ManualRouteResultV1 output;
        output.request_id = intent.request_id;
        output.correlation_id = intent.correlation_id;
        output.request_hash = intent.request_hash;

        if (!intent.valid()) {
            output.status = ManualRouteStatusV1::RiskRejected;
            output.reason = "INVALID_MANUAL_TARGET_INTENT";
            return output;
        }

        const auto account = adapter_.chaos().runtime().account().accountSnapshot(intent.decision_timestamp);
        const auto risk = risk_.evaluate(
            intent,
            account.equity,
            strategy_positions_,
            closes);

        if (!risk.approved) {
            output.status = ManualRouteStatusV1::RiskRejected;
            output.reason = risk.reason;
            return output;
        }

        ++state_revision_;
        if (state_revision_ == 0U)
            ++state_revision_;

        const std::vector<StrategyID> ids{kManualStrategyId};
        auto plan = planner_.createPlan(
            ids,
            strategy_positions_,
            order_manager_,
            intent.decision_timestamp,
            state_revision_,
            closes,
            risk.decisions,
            next_order_id_);
        next_order_id_ = plan.next_order_id;
        output.cancel_count = plan.cancel_order_ids.size();
        output.submit_count = plan.submit_orders.size();

        if (!plan.cancel_order_ids.empty())
            routeCancels(intent, plan.cancel_order_ids);

        if (plan.submit_orders.empty()) {
            output.status = ManualRouteStatusV1::Noop;
            output.reason = "TARGET_ALREADY_SATISFIED_OR_CANCEL_ONLY";
            output.reconciliation_clean = adapter_.reconcile(intent.execution_timestamp).clean();
            return output;
        }

        const auto recon = adapter_.reconcile(intent.execution_timestamp);
        output.reconciliation_clean = recon.clean();
        if (!recon.clean() || !adapter_.canRouteNewSubmit()) {
            output.status = ManualRouteStatusV1::ReconciliationBlocked;
            output.reason = "RECONCILIATION_NOT_CLEAN_OR_FRESH";
            return output;
        }

        VenueContracts::V1::SubmitOrderBatch batch;
        batch.request = makeRequest(intent, "submit");
        std::vector<ExecutionOrder> mirrors;

        for (const auto& planned : plan.submit_orders) {
            auto canonical = canonicalOrder(planned, intent.execution_timestamp, opens);
            VenueContracts::V1::SubmitOrderItem item;
            item.item_id = "order:" + std::to_string(planned.order_id);
            item.order = canonical;
            batch.items.push_back(item);
            mirrors.emplace_back(
                planned.order_id,
                planned.strategy_id,
                planned.created_at,
                intent.execution_timestamp,
                planned.coin,
                planned.side,
                canonical.quantity);
        }

        remember(batch);
        for (const auto& order : mirrors) {
            order_manager_.track(order);
            order_manager_.markSubmitted(order.order_id, intent.execution_timestamp);
        }

        last_submit_result_.reset();
        venue_.submitOrders(batch);
        output.command_dispatched = true;

        if (!last_submit_result_) {
            output.status = ManualRouteStatusV1::AmbiguousOrPending;
            output.reason = "NO_IMMEDIATE_OPERATION_RESULT_RECONCILE_BEFORE_RETRY";
            return output;
        }

        if (!operationAccepted(*last_submit_result_)) {
            output.status = ManualRouteStatusV1::VenueRejected;
            output.reason = "MOCK_VENUE_REJECTED_MANUAL_BATCH";
            return output;
        }

        output.status = ManualRouteStatusV1::Submitted;
        output.reason = "SUBMITTED_TO_MOCK_NORMAL_PIPELINE";
        return output;
    }

    MockVenueV1::ChaosOperationResultV1 processMarketBar(
        const MockVenueV1::MarketBarObservationV1& observation)
    {
        return adapter_.processMarketBar(observation);
    }

    const StrategyPositionSnapshot& strategyPositions() const { return strategy_positions_; }
    const OrderManager& orderManager() const { return order_manager_; }

private:
    MockVenueV1::MockExchangeAdapterV1& adapter_;
    CanonicalVenueAdapter& venue_;
    ManualPortfolioRiskEngineV1 risk_;
    NotionalOrderPlannerEngine planner_;
    StrategyPositionSnapshot strategy_positions_;
    OrderManager order_manager_;
    OrderID next_order_id_ = 1U;
    std::uint64_t state_revision_ = 0U;
    std::unordered_map<std::string, std::unordered_map<std::string, OrderID>> request_items_;
    std::optional<VenueContracts::V1::OperationResult> last_submit_result_;

    static std::uint64_t fillIdHash(const std::string& value)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char ch : value) {
            hash ^= static_cast<std::uint64_t>(ch);
            hash *= 1099511628211ULL;
        }
        return hash == 0U ? 1U : hash;
    }

    static double unitsToDouble(std::uint64_t units, std::size_t scale)
    {
        long double factor = 1.0L;
        for (std::size_t i = 0U; i < scale; ++i)
            factor *= 10.0L;
        return static_cast<double>(static_cast<long double>(units) / factor);
    }

    static std::uint64_t floorUnits(double value, std::size_t scale)
    {
        if (!std::isfinite(value) || value <= 0.0)
            return 0U;
        long double factor = 1.0L;
        for (std::size_t i = 0U; i < scale; ++i)
            factor *= 10.0L;
        const long double scaled = static_cast<long double>(value) * factor;
        if (scaled <= 0.0L || scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return 0U;
        return static_cast<std::uint64_t>(std::floor(scaled + 1e-12L));
    }

    static std::uint64_t ceilUnits(double value, std::size_t scale)
    {
        if (!std::isfinite(value) || value <= 0.0)
            return 0U;
        long double factor = 1.0L;
        for (std::size_t i = 0U; i < scale; ++i)
            factor *= 10.0L;
        const long double scaled = static_cast<long double>(value) * factor;
        if (scaled <= 0.0L || scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return 0U;
        return static_cast<std::uint64_t>(std::ceil(scaled - 1e-12L));
    }

    VenueContracts::V1::RequestIdentity makeRequest(
        const ManualTargetIntentV1& intent,
        const std::string& action) const
    {
        VenueContracts::V1::RequestIdentity request;
        request.venue = venue_.context();
        request.request_id = "manual:" + action + ":" + intent.request_id;
        request.correlation_id = intent.correlation_id;
        request.requested_at = intent.execution_timestamp;
        return request;
    }

    VenueContracts::V1::LimitOrderIntent canonicalOrder(
        const PlannedNotionalOrder& planned,
        Timestamp execution_timestamp,
        const ExecutionReferencePrices& opens) const
    {
        const auto* entry = MockVenueV1::findByCanonicalAssetExact(planned.coin);
        const auto* rules = entry == nullptr ? nullptr : MockVenueV1::rulesForExact(*entry);
        if (entry == nullptr || !entry->enabled || rules == nullptr || !rules->valid())
            throw std::runtime_error("manual pipeline asset not explicitly routable on MOCK");
        if (!opens.contains(planned.coin))
            throw std::runtime_error("manual pipeline missing open(T+1)");

        std::uint64_t quantity_units = 0U;
        if (planned.target_notional_usd == 0.0) {
            const auto it = strategy_positions_.find(planned.strategy_id);
            const double current = it == strategy_positions_.end() ? 0.0 : it->second.get(planned.coin);
            if (current != 0.0 && !MockVenueV1::doubleToScaledUnitsExact(
                    std::abs(current), rules->size_scale, &quantity_units))
                throw std::runtime_error("manual FLAT quantity off MOCK size grid");
        }
        if (quantity_units == 0U)
            quantity_units = floorUnits(planned.notional_usd / planned.reference_close, rules->size_scale);
        if (quantity_units == 0U)
            throw std::runtime_error("manual quantity quantized to zero");

        const double open = opens.get(planned.coin);
        const std::uint64_t price_units = planned.side == OrderSide::Buy
            ? ceilUnits(open, rules->price_scale)
            : floorUnits(open, rules->price_scale);
        if (price_units == 0U)
            throw std::runtime_error("manual execution price quantized to zero");

        VenueContracts::V1::LimitOrderIntent order;
        order.local_order_id = planned.order_id;
        order.strategy_id = planned.strategy_id;
        order.created_at = planned.created_at;
        order.active_from = execution_timestamp;
        order.instrument = entry->instrument();
        order.side = planned.side == OrderSide::Buy ? VenueContracts::V1::Side::Buy : VenueContracts::V1::Side::Sell;
        order.quantity = unitsToDouble(quantity_units, rules->size_scale);
        order.limit_price = unitsToDouble(price_units, rules->price_scale);
        order.time_in_force = VenueContracts::V1::TimeInForce::Gtc;
        order.client_order_id = planned.economic_order_id;
        if (!order.valid())
            throw std::runtime_error("manual pipeline generated invalid canonical order");
        return order;
    }

    void routeCancels(const ManualTargetIntentV1& intent, const std::vector<OrderID>& ids)
    {
        VenueContracts::V1::CancelOrderBatch batch;
        batch.request = makeRequest(intent, "cancel");
        for (const OrderID id : ids) {
            const auto* stored = adapter_.chaos().runtime().lifecycle().findOrder(id);
            if (stored == nullptr || !stored->active())
                continue;
            VenueContracts::V1::CancelOrderItem item;
            item.item_id = "order:" + std::to_string(id);
            item.order.local_order_id = id;
            item.order.instrument = stored->intent.instrument;
            item.order.native_references = stored->native_references;
            batch.items.push_back(std::move(item));
            if (const auto* tracked = order_manager_.find(id); tracked != nullptr && tracked->isOpen())
                order_manager_.markCancelRequested(id, intent.execution_timestamp);
        }
        if (!batch.items.empty()) {
            remember(batch);
            venue_.cancelOrders(batch);
        }
    }

    void remember(const VenueContracts::V1::SubmitOrderBatch& batch)
    {
        auto& dst = request_items_[batch.request.request_id];
        for (const auto& item : batch.items)
            dst.emplace(item.item_id, item.order.local_order_id);
    }

    void remember(const VenueContracts::V1::CancelOrderBatch& batch)
    {
        auto& dst = request_items_[batch.request.request_id];
        for (const auto& item : batch.items)
            dst.emplace(item.item_id, item.order.local_order_id);
    }

    static bool operationAccepted(const VenueContracts::V1::OperationResult& result)
    {
        if (result.scope == VenueContracts::V1::ResultScope::Operation)
            return false;
        if (!result.accepted || result.item_results.empty())
            return false;
        return std::all_of(result.item_results.begin(), result.item_results.end(), [](const auto& item) {
            return item.accepted;
        });
    }

    void onVenueEvent(const VenueContracts::V1::Event& event)
    {
        if (std::holds_alternative<VenueContracts::V1::OperationResult>(event)) {
            const auto& result = std::get<VenueContracts::V1::OperationResult>(event);
            if (result.command == VenueContracts::V1::CommandKind::Submit)
                last_submit_result_ = result;
            return;
        }
        if (std::holds_alternative<VenueContracts::V1::OrderUpdate>(event)) {
            onOrderUpdate(std::get<VenueContracts::V1::OrderUpdate>(event));
            return;
        }
        if (std::holds_alternative<VenueContracts::V1::Fill>(event))
            onFill(std::get<VenueContracts::V1::Fill>(event));
    }

    void onOrderUpdate(const VenueContracts::V1::OrderUpdate& update)
    {
        const auto* tracked = order_manager_.find(update.local_order_id);
        if (tracked == nullptr)
            return;

        auto apply = [&](ExecutionOrderStatus status) {
            order_manager_.onOrderUpdate(OrderUpdate(
                update.local_order_id,
                update.timestamp,
                status,
                update.native_references.native_order_id,
                update.native_status));
        };

        switch (update.status) {
        case VenueContracts::V1::OrderLifecycleStatus::PendingSubmit: break;
        case VenueContracts::V1::OrderLifecycleStatus::Accepted:
        case VenueContracts::V1::OrderLifecycleStatus::Resting: apply(ExecutionOrderStatus::Accepted); break;
        case VenueContracts::V1::OrderLifecycleStatus::PartiallyFilled: apply(ExecutionOrderStatus::PartiallyFilled); break;
        case VenueContracts::V1::OrderLifecycleStatus::Filled: apply(ExecutionOrderStatus::Filled); break;
        case VenueContracts::V1::OrderLifecycleStatus::CancelPending: break;
        case VenueContracts::V1::OrderLifecycleStatus::Canceled: apply(ExecutionOrderStatus::Canceled); break;
        case VenueContracts::V1::OrderLifecycleStatus::Rejected:
        case VenueContracts::V1::OrderLifecycleStatus::VenueTerminated: apply(ExecutionOrderStatus::Rejected); break;
        case VenueContracts::V1::OrderLifecycleStatus::UnknownRequiresReconciliation: break;
        }
    }

    void onFill(const VenueContracts::V1::Fill& fill)
    {
        Fill legacy;
        legacy.fill_id = fillIdHash(fill.native_references.native_fill_id);
        legacy.order_id = fill.local_order_id;
        legacy.strategy_id = fill.strategy_id;
        legacy.timestamp = fill.timestamp;
        legacy.coin = fill.instrument.market.canonical_asset;
        legacy.side = fill.side == VenueContracts::V1::Side::Buy ? OrderSide::Buy : OrderSide::Sell;
        legacy.quantity = fill.quantity;
        legacy.price = fill.price;
        legacy.commission = 0.0;
        if (!order_manager_.onFill(legacy))
            return;
        strategy_positions_.at(kManualStrategyId).add(legacy.coin, legacy.signedQuantity());
    }
};

} // namespace ManualControlV1
