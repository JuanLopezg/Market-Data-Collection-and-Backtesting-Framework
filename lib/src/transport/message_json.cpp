#include "message_json.h"
#include "live_execution_identity.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>


// Central JSON boundary for transport contracts.
//
// The mappings stay together because they are mechanical representations of related
// wire contracts. Helpers remain private to this translation unit so serialization rules
// are easy to audit without spreading tiny codec files across the project.
namespace {

using json = nlohmann::json;


json metadataToJson(const ContractMetadata& metadata)
{
    return {
        {"schema_version", metadata.schema_version},
        {"message_id", metadata.message_id},
        {"correlation_id", metadata.correlation_id},
        {"produced_at", metadata.produced_at}
    };
}


// Required fields use at(), so malformed messages fail instead of silently inventing
// IDs or timestamps. Contract consumers still enforce supported versions and business rules.
ContractMetadata metadataFromJson(const json& value)
{
    ContractMetadata metadata;
    metadata.schema_version = value.at("schema_version").get<std::uint32_t>();
    metadata.message_id = value.at("message_id").get<std::string>();
    metadata.correlation_id = value.at("correlation_id").get<std::string>();
    metadata.produced_at = value.at("produced_at").get<Timestamp>();
    return metadata;
}


json executionOrderToJson(const ExecutionOrder& order)
{
    if (order.entry_stop_price != 0.0 || order.protective_stop_price != 0.0 || order.parent_order_id != 0)
        throw std::invalid_argument("Service wire contract does not support stop entries");
    return {
        {"order_id", order.order_id},
        {"strategy_id", order.strategy_id},
        {"created_at", order.created_at},
        {"active_from", order.active_from},
        {"coin", order.coin},
        {"side", static_cast<int>(order.side)},
        {"quantity", order.quantity}
    };
}


ExecutionOrder executionOrderFromJson(const json& value)
{
    if (value.value("entry_stop_price", 0.0) != 0.0 || value.value("protective_stop_price", 0.0) != 0.0 || value.value("parent_order_id", OrderID{0}) != 0)
        throw std::invalid_argument("Service wire contract does not support stop entries");
    return ExecutionOrder(
        value.at("order_id").get<OrderID>(),
        value.at("strategy_id").get<StrategyID>(),
        value.at("created_at").get<Timestamp>(),
        value.at("active_from").get<Timestamp>(),
        value.at("coin").get<Coin>(),
        static_cast<OrderSide>(value.at("side").get<int>()),
        value.at("quantity").get<double>()
    );
}


json fillToJson(const Fill& fill)
{
    return {
        {"fill_id", fill.fill_id},
        {"order_id", fill.order_id},
        {"strategy_id", fill.strategy_id},
        {"timestamp", fill.timestamp},
        {"coin", fill.coin},
        {"side", static_cast<int>(fill.side)},
        {"quantity", fill.quantity},
        {"price", fill.price},
        {"commission", fill.commission}
    };
}


Fill fillFromJson(const json& value)
{
    Fill fill;
    fill.fill_id = value.at("fill_id").get<FillID>();
    fill.order_id = value.at("order_id").get<OrderID>();
    fill.strategy_id = value.at("strategy_id").get<StrategyID>();
    fill.timestamp = value.at("timestamp").get<Timestamp>();
    fill.coin = value.at("coin").get<Coin>();
    fill.side = static_cast<OrderSide>(value.at("side").get<int>());
    fill.quantity = value.at("quantity").get<double>();
    fill.price = value.at("price").get<double>();
    fill.commission = value.at("commission").get<double>();
    fill.validate();
    return fill;
}


json orderUpdateToJson(const OrderUpdate& update)
{
    return {
        {"order_id", update.order_id},
        {"timestamp", update.timestamp},
        {"status", static_cast<int>(update.status)},
        {"exchange_order_id", update.exchange_order_id},
        {"message", update.message}
    };
}


OrderUpdate orderUpdateFromJson(const json& value)
{
    return OrderUpdate(
        value.at("order_id").get<OrderID>(),
        value.at("timestamp").get<Timestamp>(),
        static_cast<ExecutionOrderStatus>(value.at("status").get<int>()),
        value.at("exchange_order_id").get<std::string>(),
        value.at("message").get<std::string>()
    );
}


json trackedOrderToJson(const TrackedOrder& order)
{
    return {
        {"request", executionOrderToJson(order.request)},
        {"status", static_cast<int>(order.status)},
        {"filled_quantity", order.filled_quantity},
        {"updated_at", order.updated_at},
        {"cancel_requested", order.cancel_requested},
        {"exchange_order_id", order.exchange_order_id},
        {"last_message", order.last_message}
    };
}


TrackedOrder trackedOrderFromJson(const json& value)
{
    TrackedOrder result(executionOrderFromJson(value.at("request")));
    result.status = static_cast<ExecutionOrderStatus>(value.at("status").get<int>());
    result.filled_quantity = value.at("filled_quantity").get<double>();
    result.updated_at = value.at("updated_at").get<Timestamp>();
    result.cancel_requested = value.at("cancel_requested").get<bool>();
    result.exchange_order_id = value.at("exchange_order_id").get<std::string>();
    result.last_message = value.at("last_message").get<std::string>();
    return result;
}

template <typename T>
std::string dump(const T& value)
{
    return value.dump();
}

}


namespace MessageJson {

std::string encode(const MarketDataReleaseRequest& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"kind", static_cast<int>(value.kind)},
        {"timestamp", value.timestamp},
        {"decision_timestamp", value.decision_timestamp}
    });
}

MarketDataReleaseRequest decodeMarketDataReleaseRequest(const std::string& payload)
{
    const json value = json::parse(payload);
    MarketDataReleaseRequest result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.kind = static_cast<MarketDataReleaseKind>(value.at("kind").get<int>());
    result.timestamp = value.at("timestamp").get<Timestamp>();
    result.decision_timestamp = value.value("decision_timestamp", Timestamp{0});

    if (result.kind != MarketDataReleaseKind::ClosedSlice &&
        result.kind != MarketDataReleaseKind::ExecutionOpen)
        throw std::invalid_argument("Invalid market-data release kind");

    return result;
}


std::string encode(const MarketDataUpdated& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"completed_through", value.completed_through},
        {"source", value.source},
        {"timeframe", value.timeframe},
        {"ranked_symbols", value.ranked_symbols},
        {"active_top_n", value.active_top_n},
        {"tracked_symbols", value.tracked_symbols},
        {"maintained_symbols", value.maintained_symbols},
        {"requested_symbols", value.requested_symbols},
        {"downloaded_rows", value.downloaded_rows}
    });
}

MarketDataUpdated decodeMarketDataUpdated(const std::string& payload)
{
    const json value = json::parse(payload);
    MarketDataUpdated result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.completed_through = value.at("completed_through").get<Timestamp>();
    result.source = value.at("source").get<std::string>();
    result.timeframe = value.at("timeframe").get<std::string>();
    result.ranked_symbols = value.value("ranked_symbols", std::uint64_t{0});
    result.active_top_n = value.value("active_top_n", std::uint64_t{0});
    result.tracked_symbols = value.value("tracked_symbols", std::uint64_t{0});
    result.maintained_symbols = value.value("maintained_symbols", std::uint64_t{0});
    result.requested_symbols = value.value("requested_symbols", std::uint64_t{0});
    result.downloaded_rows = value.value("downloaded_rows", std::uint64_t{0});

    if (result.metadata.schema_version != 1 ||
        result.metadata.message_id.empty() ||
        result.completed_through == 0 ||
        result.source.empty() ||
        result.timeframe.empty())
        throw std::invalid_argument("Invalid market-data updated event");

    return result;
}


std::string encode(const MarketSliceClosed& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"timestamp", value.timestamp},
        {"symbols", value.symbols}
    });
}

MarketSliceClosed decodeMarketSliceClosed(const std::string& payload)
{
    const json value = json::parse(payload);
    MarketSliceClosed result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.timestamp = value.at("timestamp").get<Timestamp>();
    result.symbols = value.at("symbols").get<std::vector<Coin>>();
    return result;
}


std::string encode(const MarketSliceSnapshot& value)
{
    json bars = json::array();
    for (const MarketBarSnapshot& item : value.bars) {
        bars.push_back({
            {"coin", item.coin},
            {"open", item.bar.open},
            {"high", item.bar.high},
            {"low", item.bar.low},
            {"close", item.bar.close},
            {"volume", item.bar.volume}
        });
        if (std::isfinite(item.bar.quote_volume))
            bars.back()["quote_volume"] = item.bar.quote_volume;
    }

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"timestamp", value.timestamp},
        {"bars", std::move(bars)}
    });
}

MarketSliceSnapshot decodeMarketSliceSnapshot(const std::string& payload)
{
    const json value = json::parse(payload);
    MarketSliceSnapshot result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.timestamp = value.at("timestamp").get<Timestamp>();

    for (const json& barValue : value.at("bars")) {
        MarketBarSnapshot item;
        item.coin = barValue.at("coin").get<Coin>();
        item.bar.open = barValue.at("open").get<double>();
        item.bar.high = barValue.at("high").get<double>();
        item.bar.low = barValue.at("low").get<double>();
        item.bar.close = barValue.at("close").get<double>();
        item.bar.volume = barValue.at("volume").get<double>();
        if (barValue.contains("quote_volume") && !barValue.at("quote_volume").is_null())
            item.bar.quote_volume = barValue.at("quote_volume").get<double>();
        result.bars.push_back(std::move(item));
    }

    return result;
}


std::string encode(const StrategyIntentBatch& value)
{
    json strategies = json::array();
    for (const StrategySignalIntent& strategy : value.strategies) {
        strategies.push_back({
            {"strategy_id", strategy.strategy_id},
            {"strategy_name", strategy.strategy_name},
            {"signals", strategy.signals}
        });
    }

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"timestamp", value.timestamp},
        {"strategies", std::move(strategies)}
    });
}

StrategyIntentBatch decodeStrategyIntentBatch(const std::string& payload)
{
    const json value = json::parse(payload);
    StrategyIntentBatch result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.timestamp = value.at("timestamp").get<Timestamp>();

    for (const json& strategyValue : value.at("strategies")) {
        StrategySignalIntent strategy;
        strategy.strategy_id = strategyValue.at("strategy_id").get<StrategyID>();
        strategy.strategy_name = strategyValue.at("strategy_name").get<std::string>();
        strategy.signals = strategyValue.at("signals").get<std::unordered_map<Coin, double>>();

        for (const auto& [coin, signal] : strategy.signals) {
            if (coin.empty() || !std::isfinite(signal) || signal < -1.0 || signal > 1.0)
                throw std::invalid_argument("Invalid strategy signal payload");
        }

        result.strategies.push_back(std::move(strategy));
    }

    return result;
}


std::string encode(const DecisionBatch& value)
{
    json strategies = json::array();
    for (const StrategyDecisionIntent& strategy : value.strategies) {
        json decisions = json::array();
        for (const auto& [coin, decision] : strategy.decisions) {
            if (decision.entry_stop_price != 0.0 || decision.protective_stop_price != 0.0)
                throw std::invalid_argument("Service wire contract does not support stop intents");
            decisions.push_back({
                {"coin", coin},
                {"action", static_cast<int>(decision.action)},
                {"target_weight", decision.target_weight}
            });
        }

        strategies.push_back({
            {"strategy_id", strategy.strategy_id},
            {"decision_timestamp", strategy.decision_timestamp},
            {"reference_capital", strategy.reference_capital},
            {"target_notional_usd", strategy.target_notional_usd},
            {"decisions", std::move(decisions)}
        });
    }

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"strategies", std::move(strategies)}
    });
}

DecisionBatch decodeDecisionBatch(const std::string& payload)
{
    const json value = json::parse(payload);
    DecisionBatch result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();

    for (const json& strategyValue : value.at("strategies")) {
        StrategyDecisionIntent strategy;
        strategy.strategy_id = strategyValue.at("strategy_id").get<StrategyID>();
        strategy.decision_timestamp = strategyValue.at("decision_timestamp").get<Timestamp>();
        strategy.reference_capital = strategyValue.at("reference_capital").get<double>();
        strategy.target_notional_usd = strategyValue.value(
            "target_notional_usd",
            std::unordered_map<Coin, double>{}
        );

        for (const json& decisionValue : strategyValue.at("decisions")) {
            if (decisionValue.value("entry_stop_price", 0.0) != 0.0 || decisionValue.value("protective_stop_price", 0.0) != 0.0)
                throw std::invalid_argument("Service wire contract does not support stop intents");
            RebalanceDecision decision;
            decision.action = static_cast<RebalanceAction>(decisionValue.at("action").get<int>());
            decision.target_weight = decisionValue.at("target_weight").get<double>();
            strategy.decisions.emplace(decisionValue.at("coin").get<Coin>(), decision);
        }

        result.strategies.push_back(std::move(strategy));
    }

    return result;
}


std::string encode(const ExecutionPriceSnapshot& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"timestamp", value.timestamp},
        {"decision_timestamp", value.decision_timestamp},
        {"prices", value.prices}
    });
}

ExecutionPriceSnapshot decodeExecutionPriceSnapshot(const std::string& payload)
{
    const json value = json::parse(payload);
    ExecutionPriceSnapshot result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.timestamp = value.at("timestamp").get<Timestamp>();
    result.decision_timestamp = value.value("decision_timestamp", Timestamp{0});
    result.prices = value.at("prices").get<std::unordered_map<Coin, double>>();
    return result;
}


std::string encode(const SubmitOrderCommand& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"order", executionOrderToJson(value.order)}
    });
}

SubmitOrderCommand decodeSubmitOrderCommand(const std::string& payload)
{
    const json value = json::parse(payload);
    SubmitOrderCommand result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.order = executionOrderFromJson(value.at("order"));
    return result;
}


std::string encode(const CancelOrderCommand& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"order_id", value.order_id},
        {"requested_at", value.requested_at}
    });
}

CancelOrderCommand decodeCancelOrderCommand(const std::string& payload)
{
    const json value = json::parse(payload);
    CancelOrderCommand result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.order_id = value.at("order_id").get<OrderID>();
    result.requested_at = value.at("requested_at").get<Timestamp>();
    return result;
}


std::string encode(const OrderUpdateEvent& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"update", orderUpdateToJson(value.update)}
    });
}

OrderUpdateEvent decodeOrderUpdateEvent(const std::string& payload)
{
    const json value = json::parse(payload);
    OrderUpdateEvent result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.update = orderUpdateFromJson(value.at("update"));
    return result;
}


std::string encode(const FillEvent& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"fill", fillToJson(value.fill)}
    });
}

FillEvent decodeFillEvent(const std::string& payload)
{
    const json value = json::parse(payload);
    FillEvent result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.fill = fillFromJson(value.at("fill"));
    return result;
}


std::string encode(const AccountSnapshot& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"timestamp", value.timestamp},
        {"cash", value.cash},
        {"positions", value.positions},
        {"strategy_positions", value.strategy_positions}
    });
}

AccountSnapshot decodeAccountSnapshot(const std::string& payload)
{
    const json value = json::parse(payload);
    AccountSnapshot result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.timestamp = value.at("timestamp").get<Timestamp>();
    result.cash = value.at("cash").get<double>();
    result.positions = value.at("positions").get<std::unordered_map<Coin, double>>();
    result.strategy_positions = value.at("strategy_positions").get<
        std::unordered_map<StrategyID, std::unordered_map<Coin, double>>
    >();
    return result;
}


std::string encode(const ExecutionCycleComplete& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"execution_timestamp", value.execution_timestamp},
        {"state_revision", value.state_revision}
    });
}

ExecutionCycleComplete decodeExecutionCycleComplete(const std::string& payload)
{
    const json value = json::parse(payload);
    ExecutionCycleComplete result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();
    result.execution_timestamp = value.at("execution_timestamp").get<Timestamp>();
    result.state_revision = value.at("state_revision").get<std::uint64_t>();
    return result;
}


std::string encode(const ExchangeSnapshotEvent& value)
{
    json openOrders = json::array();
    for (const ExchangeOpenOrderSnapshot& order : value.snapshot.open_orders) {
        openOrders.push_back({
            {"local_order_id", order.local_order_id},
            {"exchange_order_id", order.exchange_order_id},
            {"coin", order.coin},
            {"side", static_cast<int>(order.side)},
            {"quantity", order.quantity},
            {"filled_quantity", order.filled_quantity}
        });
    }

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"snapshot", {
            {"timestamp", value.snapshot.timestamp},
            {"cash", value.snapshot.cash},
            {"positions", value.snapshot.positions},
            {"open_orders", std::move(openOrders)}
        }}
    });
}

ExchangeSnapshotEvent decodeExchangeSnapshotEvent(const std::string& payload)
{
    const json value = json::parse(payload);
    ExchangeSnapshotEvent result;
    result.metadata = metadataFromJson(value.at("metadata"));

    const json& snapshot = value.at("snapshot");
    result.snapshot.timestamp = snapshot.at("timestamp").get<Timestamp>();
    result.snapshot.cash = snapshot.at("cash").get<double>();
    result.snapshot.positions = snapshot.at("positions").get<std::unordered_map<Coin, double>>();

    for (const json& item : snapshot.at("open_orders")) {
        ExchangeOpenOrderSnapshot order;
        order.local_order_id = item.at("local_order_id").get<OrderID>();
        order.exchange_order_id = item.at("exchange_order_id").get<std::string>();
        order.coin = item.at("coin").get<Coin>();
        order.side = static_cast<OrderSide>(item.at("side").get<int>());
        order.quantity = item.at("quantity").get<double>();
        order.filled_quantity = item.at("filled_quantity").get<double>();
        result.snapshot.open_orders.push_back(std::move(order));
    }
    return result;
}


std::string encode(const ExchangeSnapshotRequest& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)}
    });
}

ExchangeSnapshotRequest decodeExchangeSnapshotRequest(const std::string& payload)
{
    const json value = json::parse(payload);
    ExchangeSnapshotRequest result;
    result.metadata = metadataFromJson(value.at("metadata"));
    return result;
}


std::string encode(const OrderPlanningRequest& value)
{
    json strategyPositions = json::array();
    for (const auto& [strategyId, positions] : value.state.strategy_positions) {
        strategyPositions.push_back({
            {"strategy_id", strategyId},
            {"positions", positions}
        });
    }

    json orders = json::array();
    for (const TrackedOrder& order : value.state.orders)
        orders.push_back(trackedOrderToJson(order));

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"execution_timestamp", value.execution_timestamp},
        {"decisions", json::parse(encode(value.decisions))},
        {"prices", json::parse(encode(value.prices))},
        {"state", {
            {"state_revision", value.state.state_revision},
            {"strategy_ids", value.state.strategy_ids},
            {"strategy_positions", std::move(strategyPositions)},
            {"orders", std::move(orders)},
            {"next_order_id", value.state.next_order_id}
        }}
    });
}

OrderPlanningRequest decodeOrderPlanningRequest(const std::string& payload)
{
    const json value = json::parse(payload);
    OrderPlanningRequest result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();
    result.execution_timestamp = value.at("execution_timestamp").get<Timestamp>();
    result.decisions = decodeDecisionBatch(value.at("decisions").dump());
    result.prices = decodeExecutionPriceSnapshot(value.at("prices").dump());

    const json& state = value.at("state");
    result.state.state_revision = state.at("state_revision").get<std::uint64_t>();
    result.state.strategy_ids = state.at("strategy_ids").get<std::vector<StrategyID>>();
    result.state.next_order_id = state.at("next_order_id").get<OrderID>();

    for (const json& item : state.at("strategy_positions")) {
        const StrategyID strategyId = item.at("strategy_id").get<StrategyID>();
        if (!result.state.strategy_positions.emplace(
                strategyId,
                item.at("positions").get<std::unordered_map<Coin, double>>()
            ).second)
            throw std::invalid_argument("Duplicate strategy position id in planning request");
    }

    for (const json& item : state.at("orders"))
        result.state.orders.push_back(trackedOrderFromJson(item));

    return result;
}


std::string encode(const OrderPlanBatch& value)
{
    json orders = json::array();
    for (const ExecutionOrder& order : value.submit_orders)
        orders.push_back(executionOrderToJson(order));

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"execution_timestamp", value.execution_timestamp},
        {"state_revision", value.state_revision},
        {"decisions", json::parse(encode(value.decisions))},
        {"prices", json::parse(encode(value.prices))},
        {"next_order_id", value.next_order_id},
        {"cancel_order_ids", value.cancel_order_ids},
        {"submit_orders", std::move(orders)},
        {"global_target_exposure", value.global_target_exposure}
    });
}

OrderPlanBatch decodeOrderPlanBatch(const std::string& payload)
{
    const json value = json::parse(payload);
    OrderPlanBatch result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();
    result.execution_timestamp = value.at("execution_timestamp").get<Timestamp>();
    result.state_revision = value.at("state_revision").get<std::uint64_t>();
    result.decisions = decodeDecisionBatch(value.at("decisions").dump());
    result.prices = decodeExecutionPriceSnapshot(value.at("prices").dump());
    result.next_order_id = value.at("next_order_id").get<OrderID>();
    result.cancel_order_ids = value.at("cancel_order_ids").get<std::vector<OrderID>>();
    result.global_target_exposure =
        value.at("global_target_exposure").get<std::unordered_map<Coin, double>>();

    for (const json& item : value.at("submit_orders"))
        result.submit_orders.push_back(executionOrderFromJson(item));

    return result;
}


std::string encode(const DailyCloseSnapshot& value)
{
    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"date", value.date},
        {"closes", value.closes}
    });
}

DailyCloseSnapshot decodeDailyCloseSnapshot(const std::string& payload)
{
    const json value = json::parse(payload);
    DailyCloseSnapshot result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.date = value.at("date").get<Timestamp>();
    result.closes = value.at("closes").get<std::unordered_map<Coin, double>>();
    return result;
}

std::string encode(const NotionalOrderPlanningRequest& value)
{
    json strategyPositions = json::array();
    for (const auto& [strategyId, positions] : value.state.strategy_positions) {
        strategyPositions.push_back({
            {"strategy_id", strategyId},
            {"positions", positions}
        });
    }

    json orders = json::array();
    for (const TrackedOrder& order : value.state.orders)
        orders.push_back(trackedOrderToJson(order));

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"decisions", json::parse(encode(value.decisions))},
        {"reference_closes", json::parse(encode(value.reference_closes))},
        {"state", {
            {"state_revision", value.state.state_revision},
            {"strategy_ids", value.state.strategy_ids},
            {"strategy_positions", std::move(strategyPositions)},
            {"orders", std::move(orders)},
            {"next_order_id", value.state.next_order_id}
        }}
    });
}

NotionalOrderPlanningRequest decodeNotionalOrderPlanningRequest(const std::string& payload)
{
    const json value = json::parse(payload);
    NotionalOrderPlanningRequest result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();
    result.decisions = decodeDecisionBatch(value.at("decisions").dump());
    result.reference_closes = decodeDailyCloseSnapshot(value.at("reference_closes").dump());

    const json& state = value.at("state");
    result.state.state_revision = state.at("state_revision").get<std::uint64_t>();
    result.state.strategy_ids = state.at("strategy_ids").get<std::vector<StrategyID>>();
    result.state.next_order_id = state.at("next_order_id").get<OrderID>();

    for (const json& item : state.at("strategy_positions")) {
        const StrategyID strategyId = item.at("strategy_id").get<StrategyID>();
        if (!result.state.strategy_positions.emplace(
                strategyId,
                item.at("positions").get<std::unordered_map<Coin, double>>()
            ).second)
            throw std::invalid_argument("Duplicate strategy position id in notional planning request");
    }

    for (const json& item : state.at("orders"))
        result.state.orders.push_back(trackedOrderFromJson(item));

    return result;
}

std::string encode(const NotionalOrderPlanBatch& value)
{
    json orders = json::array();
    for (const PlannedNotionalOrder& order : value.submit_orders) {
        orders.push_back({
            {"economic_order_id", order.economic_order_id},
            {"order_id", order.order_id},
            {"strategy_id", order.strategy_id},
            {"created_at", order.created_at},
            {"decision_timestamp", order.decision_timestamp},
            {"state_revision", order.state_revision},
            {"coin", order.coin},
            {"side", static_cast<int>(order.side)},
            {"reference_close", order.reference_close},
            {"target_notional_usd", order.target_notional_usd},
            {"current_notional_usd", order.current_notional_usd},
            {"pending_notional_usd", order.pending_notional_usd},
            {"delta_notional_usd", order.delta_notional_usd},
            {"notional_usd", order.notional_usd}
        });
    }

    return dump(json{
        {"metadata", metadataToJson(value.metadata)},
        {"decision_timestamp", value.decision_timestamp},
        {"state_revision", value.state_revision},
        {"decisions", json::parse(encode(value.decisions))},
        {"reference_closes", json::parse(encode(value.reference_closes))},
        {"next_order_id", value.next_order_id},
        {"cancel_order_ids", value.cancel_order_ids},
        {"submit_orders", std::move(orders)},
        {"global_target_notional_usd", value.global_target_notional_usd}
    });
}

NotionalOrderPlanBatch decodeNotionalOrderPlanBatch(const std::string& payload)
{
    const json value = json::parse(payload);
    NotionalOrderPlanBatch result;
    result.metadata = metadataFromJson(value.at("metadata"));
    result.decision_timestamp = value.at("decision_timestamp").get<Timestamp>();
    result.state_revision = value.at("state_revision").get<std::uint64_t>();
    result.decisions = decodeDecisionBatch(value.at("decisions").dump());
    result.reference_closes = decodeDailyCloseSnapshot(value.at("reference_closes").dump());
    result.next_order_id = value.at("next_order_id").get<OrderID>();
    result.cancel_order_ids = value.at("cancel_order_ids").get<std::vector<OrderID>>();
    result.global_target_notional_usd =
        value.at("global_target_notional_usd").get<std::unordered_map<Coin, double>>();

    if (result.metadata.schema_version != 1 || result.metadata.message_id.empty() ||
        result.decision_timestamp == 0 || result.state_revision == 0 ||
        result.decisions.decision_timestamp != result.decision_timestamp ||
        result.reference_closes.date != result.decision_timestamp ||
        result.metadata.message_id != LiveExecutionIdentity::notionalOrderPlan(
            result.decision_timestamp, result.state_revision))
        throw std::invalid_argument("Invalid notional order plan identity payload");

    for (const json& item : value.at("submit_orders")) {
        PlannedNotionalOrder order;
        order.economic_order_id = item.at("economic_order_id").get<std::string>();
        order.order_id = item.at("order_id").get<OrderID>();
        order.strategy_id = item.at("strategy_id").get<StrategyID>();
        order.created_at = item.at("created_at").get<Timestamp>();
        order.decision_timestamp = item.at("decision_timestamp").get<Timestamp>();
        order.state_revision = item.at("state_revision").get<std::uint64_t>();
        order.coin = item.at("coin").get<Coin>();
        order.side = static_cast<OrderSide>(item.at("side").get<int>());
        order.reference_close = item.at("reference_close").get<double>();
        order.target_notional_usd = item.at("target_notional_usd").get<double>();
        order.current_notional_usd = item.at("current_notional_usd").get<double>();
        order.pending_notional_usd = item.at("pending_notional_usd").get<double>();
        order.delta_notional_usd = item.at("delta_notional_usd").get<double>();
        order.notional_usd = item.at("notional_usd").get<double>();

        if (order.economic_order_id.empty() || order.order_id == 0 || order.strategy_id == 0 ||
            order.created_at == 0 || order.decision_timestamp != result.decision_timestamp ||
            order.state_revision != result.state_revision || order.coin.empty())
            throw std::invalid_argument("Invalid planned notional order identity payload");
        if (!std::isfinite(order.reference_close) || order.reference_close <= 0.0 ||
            !std::isfinite(order.target_notional_usd) ||
            !std::isfinite(order.current_notional_usd) ||
            !std::isfinite(order.pending_notional_usd) ||
            !std::isfinite(order.delta_notional_usd) ||
            !std::isfinite(order.notional_usd) || order.notional_usd <= 0.0)
            throw std::invalid_argument("Invalid planned notional order economic payload");

        const int side = static_cast<int>(order.side);
        if (side != static_cast<int>(OrderSide::Buy) && side != static_cast<int>(OrderSide::Sell))
            throw std::invalid_argument("Invalid planned notional order side");
        const double deltaMagnitude = std::abs(order.delta_notional_usd);
        const double deltaTolerance = 1e-9 * std::max(1.0, deltaMagnitude);
        if ((order.delta_notional_usd > 0.0) != (order.side == OrderSide::Buy) ||
            std::abs(deltaMagnitude - order.notional_usd) > deltaTolerance)
            throw std::invalid_argument("Planned notional order side/delta mismatch");
        if (order.economic_order_id != LiveExecutionIdentity::plannedEconomicOrder(
                result.decision_timestamp, result.state_revision, order.order_id))
            throw std::invalid_argument("Planned notional order deterministic identity mismatch");

        const auto closeIt = result.reference_closes.closes.find(order.coin);
        if (closeIt == result.reference_closes.closes.end() || closeIt->second != order.reference_close)
            throw std::invalid_argument("Planned notional order reference close mismatch");

        result.submit_orders.push_back(std::move(order));
    }

    return result;
}

}
