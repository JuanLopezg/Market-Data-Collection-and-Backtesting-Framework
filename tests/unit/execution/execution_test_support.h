#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "execution_engine.h"
#include "fake_exchange.h"

namespace execution_test {

inline constexpr Timestamp kClose = 20260105;
inline constexpr Timestamp kOpen = 20260106;
inline constexpr Timestamp kLater = 20260107;
inline constexpr StrategyID kAlpha = 7;
inline constexpr StrategyID kBeta = 9;

inline ExecutionOrder order(OrderID id = 10, OrderSide side = OrderSide::Buy,
                            double quantity = 5.0, StrategyID strategy = kAlpha)
{
    return ExecutionOrder(id, strategy, kClose, kOpen, "BTC", side, quantity);
}

inline Fill fill(const ExecutionOrder& request, double quantity = 2.0,
                 double price = 100.0, double commission = 2.0, FillID id = 101)
{
    Fill result;
    result.fill_id = id;
    result.order_id = request.order_id;
    result.strategy_id = request.strategy_id;
    result.timestamp = kOpen;
    result.coin = request.coin;
    result.side = request.side;
    result.quantity = quantity;
    result.price = price;
    result.commission = commission;
    return result;
}

inline void submit(OrderManager& manager, const ExecutionOrder& request)
{
    manager.track(request);
    manager.markSubmitted(request.order_id, kOpen);
}

inline std::vector<TrackedOrder> trackedOrders(const OrderManager& manager)
{
    std::vector<TrackedOrder> result;
    for (const auto& [id, tracked] : manager.orders()) {
        (void)id;
        result.push_back(tracked);
    }
    return result;
}

// The real FakeExchange supplies lifecycle/account behaviour. This outside-world
// decorator adds a command trace, explicit capabilities and deterministic send failure.
class ObservedExchange final : public Exchange {
public:
    struct Command {
        bool submit;
        OrderID id;
    };

    FakeExchange venue;
    std::vector<Command> commands;
    OrderID fail_submit_id = 0;

    explicit ObservedExchange(double cash, double rate) : venue(cash, rate) {}
    bool supportsStopEntries() const override { return true; }
    bool supportsShortBrackets() const override { return true; }

    void submitOrder(const ExecutionOrder& request) override
    {
        if (request.order_id == fail_submit_id)
            throw std::runtime_error("Injected exchange dispatch failure");
        venue.submitOrder(request);
        commands.push_back({true, request.order_id});
    }

    void cancelOrder(OrderID id) override
    {
        venue.cancelOrder(id);
        commands.push_back({false, id});
    }

    std::vector<ExchangeEvent> drainEvents() override { return venue.drainEvents(); }
};

// Value snapshots model the caller-owned persistence boundary. They capture real
// economic/order state; no expected cash/quantity is calculated in this helper.
struct SavedState {
    std::optional<Fill> new_fill;
    double cash = 0.0;
    std::unordered_map<Coin, double> account_positions;
    StrategyPositionSnapshot strategy_positions;
    std::vector<TrackedOrder> orders;
    std::vector<FillID> fill_ids;
    OrderID next_id = 1;
    Timestamp last_execution = 0;
};

template <class Venue = FakeExchange>
struct Harness {
    Account account{1000.0};
    TradeRecorder trades;
    Venue exchange{1000.0, 0.01};
    std::vector<ExecutionStrategyDescriptor> strategies{
        {kAlpha, "Alpha", {}}, {kBeta, "Beta", {}}};
    ExecutionEngine engine{strategies, account, trades, exchange};
    std::vector<SavedState> saved;

    void capture(const std::optional<Fill>& incoming)
    {
        SavedState state;
        state.new_fill = incoming;
        state.cash = account.cash();
        state.account_positions = account.positions().values();
        state.strategy_positions = engine.strategyPositions();
        state.orders = trackedOrders(engine.orderManager());
        state.fill_ids = engine.orderManager().processedFillIds();
        state.next_id = engine.nextOrderId();
        state.last_execution = engine.lastExecutionTimestamp();
        saved.push_back(std::move(state));
    }

    ExecutionEngine::PersistCallback persist()
    {
        return [this](const std::optional<Fill>& incoming) { capture(incoming); };
    }

    void restore(const SavedState& state)
    {
        engine.restoreState(state.cash, state.account_positions, state.strategy_positions,
            state.orders, state.fill_ids, state.next_id, state.last_execution);
    }

    void manual(const ExecutionOrder& request)
    {
        engine.applyManualOrders({request}, request.order_id + 1, kOpen, persist());
    }
};

inline DecisionBatch target(double weight, Timestamp decisionAt = kClose)
{
    StrategyDecisionIntent intent;
    intent.strategy_id = kAlpha;
    intent.decision_timestamp = decisionAt;
    intent.reference_capital = 1000.0;
    intent.decisions["BTC"] = RebalanceDecision::targetWeight(weight);
    intent.target_notional_usd["BTC"] = weight * 1000.0;
    DecisionBatch decisions;
    decisions.decision_timestamp = decisionAt;
    decisions.strategies.push_back(std::move(intent));
    return decisions;
}

inline OrderPlanBatch plan(std::vector<ExecutionOrder> submits, OrderID nextId)
{
    OrderPlanBatch result;
    result.decision_timestamp = kClose;
    result.execution_timestamp = kOpen;
    result.submit_orders = std::move(submits);
    result.next_order_id = nextId;
    return result;
}

inline PriceSnapshot marks(double btc = 100.0)
{
    PriceSnapshot prices;
    prices.set("BTC", btc);
    return prices;
}

} // namespace execution_test
