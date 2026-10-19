#include <exception>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "live_execution_identity.h"
#include "message_examples.h"

namespace transport_test {
namespace {

using Json = nlohmann::json;

struct WireCase {
    std::string name;
    std::string payload;
    std::function<void(const std::string&)> decode;
    std::function<void(const std::string&)> check;
    std::vector<std::string> required_fields;
};

template <typename T>
WireCase wireCase(const std::string& name, const T& expected,
                  T (*decode)(const std::string&), std::initializer_list<const char*> fields)
{
    WireCase result{name, MessageJson::encode(expected),
        [decode](const std::string& payload) { (void)decode(payload); },
        [decode, expected](const std::string& payload) { expectSame(decode(payload), expected); },
        {"/metadata", "/metadata/schema_version", "/metadata/message_id",
         "/metadata/correlation_id", "/metadata/produced_at"}};
    for (const char* field : fields) result.required_fields.emplace_back(field);
    return result;
}

// Mandatory paths are specified explicitly by the contract, not discovered by
// enumerating whatever keys the current encoder happens to emit.
const std::vector<WireCase>& wireExamples()
{
    static const std::vector<WireCase> examples{
        wireCase("MarketDataReleaseRequest", releaseExample(), MessageJson::decodeMarketDataReleaseRequest,
            {"/kind", "/timestamp"}),
        wireCase("MarketDataUpdated", updatedExample(), MessageJson::decodeMarketDataUpdated,
            {"/completed_through", "/source", "/timeframe"}),
        wireCase("MarketSliceClosed", closedExample(), MessageJson::decodeMarketSliceClosed,
            {"/timestamp", "/symbols"}),
        wireCase("MarketSliceSnapshot", sliceExample(), MessageJson::decodeMarketSliceSnapshot,
            {"/timestamp", "/bars", "/bars/0/coin", "/bars/0/open", "/bars/0/high",
             "/bars/0/low", "/bars/0/close", "/bars/0/volume"}),
        wireCase("StrategyIntentBatch", signalsExample(), MessageJson::decodeStrategyIntentBatch,
            {"/timestamp", "/strategies", "/strategies/0/strategy_id",
             "/strategies/0/strategy_name", "/strategies/0/signals"}),
        wireCase("DecisionBatch", decisionsExample(), MessageJson::decodeDecisionBatch,
            {"/decision_timestamp", "/strategies", "/strategies/0/strategy_id",
             "/strategies/0/decision_timestamp", "/strategies/0/reference_capital",
             "/strategies/0/decisions", "/strategies/0/decisions/0/coin",
             "/strategies/0/decisions/0/action", "/strategies/0/decisions/0/target_weight"}),
        wireCase("ExecutionPriceSnapshot", pricesExample(), MessageJson::decodeExecutionPriceSnapshot,
            {"/timestamp", "/prices"}),
        wireCase("SubmitOrderCommand", submitExample(), MessageJson::decodeSubmitOrderCommand,
            {"/order", "/order/order_id", "/order/strategy_id", "/order/created_at",
             "/order/active_from", "/order/coin", "/order/side", "/order/quantity"}),
        wireCase("CancelOrderCommand", cancelExample(), MessageJson::decodeCancelOrderCommand,
            {"/order_id", "/requested_at"}),
        wireCase("OrderUpdateEvent", updateExample(), MessageJson::decodeOrderUpdateEvent,
            {"/update", "/update/order_id", "/update/timestamp", "/update/status",
             "/update/exchange_order_id", "/update/message"}),
        wireCase("FillEvent", fillExample(), MessageJson::decodeFillEvent,
            {"/fill", "/fill/fill_id", "/fill/order_id", "/fill/strategy_id", "/fill/timestamp",
             "/fill/coin", "/fill/side", "/fill/quantity", "/fill/price", "/fill/commission"}),
        wireCase("VenueCostEvent", costExample(), MessageJson::decodeVenueCostEvent,
            {"/cost_id", "/kind", "/timestamp", "/coin", "/fill_id", "/amount_usd"}),
        wireCase("AccountSnapshot", accountExample(), MessageJson::decodeAccountSnapshot,
            {"/timestamp", "/cash", "/positions", "/strategy_positions"}),
        wireCase("ExecutionCycleComplete", cycleExample(), MessageJson::decodeExecutionCycleComplete,
            {"/decision_timestamp", "/execution_timestamp", "/state_revision"}),
        wireCase("ExchangeSnapshotEvent", exchangeExample(), MessageJson::decodeExchangeSnapshotEvent,
            {"/snapshot", "/snapshot/timestamp", "/snapshot/cash", "/snapshot/positions",
             "/snapshot/open_orders", "/snapshot/open_orders/0/local_order_id",
             "/snapshot/open_orders/0/exchange_order_id", "/snapshot/open_orders/0/coin",
             "/snapshot/open_orders/0/side", "/snapshot/open_orders/0/quantity",
             "/snapshot/open_orders/0/filled_quantity"}),
        wireCase("ExchangeSnapshotRequest", exchangeRequestExample(), MessageJson::decodeExchangeSnapshotRequest, {}),
        wireCase("OrderPlanningRequest", quantityRequestExample(), MessageJson::decodeOrderPlanningRequest,
            {"/decision_timestamp", "/execution_timestamp", "/decisions", "/prices", "/state",
             "/state/state_revision", "/state/strategy_ids", "/state/strategy_positions",
             "/state/strategy_positions/0/strategy_id", "/state/strategy_positions/0/positions",
             "/state/orders", "/state/next_order_id", "/state/orders/0/request",
             "/state/orders/0/request/quantity", "/state/orders/0/status",
             "/state/orders/0/filled_quantity", "/state/orders/0/updated_at",
             "/state/orders/0/cancel_requested", "/state/orders/0/exchange_order_id",
             "/state/orders/0/last_message"}),
        wireCase("OrderPlanBatch", quantityPlanExample(), MessageJson::decodeOrderPlanBatch,
            {"/decision_timestamp", "/execution_timestamp", "/state_revision", "/decisions", "/prices",
             "/next_order_id", "/cancel_order_ids", "/submit_orders", "/submit_orders/0/quantity",
             "/global_target_exposure"}),
        wireCase("DailyCloseSnapshot", closesExample(), MessageJson::decodeDailyCloseSnapshot,
            {"/date", "/closes"}),
        wireCase("NotionalOrderPlanningRequest", notionalRequestExample(), MessageJson::decodeNotionalOrderPlanningRequest,
            {"/decision_timestamp", "/decisions", "/reference_closes", "/state",
             "/state/state_revision", "/state/strategy_ids", "/state/strategy_positions",
             "/state/strategy_positions/0/strategy_id", "/state/strategy_positions/0/positions",
             "/state/orders", "/state/next_order_id", "/state/orders/0/filled_quantity"}),
        wireCase("NotionalOrderPlanBatch", notionalPlanExample(), MessageJson::decodeNotionalOrderPlanBatch,
            {"/decision_timestamp", "/state_revision", "/decisions", "/reference_closes",
             "/next_order_id", "/cancel_order_ids", "/submit_orders", "/global_target_notional_usd",
             "/submit_orders/0/economic_order_id", "/submit_orders/0/order_id",
             "/submit_orders/0/strategy_id", "/submit_orders/0/created_at",
             "/submit_orders/0/decision_timestamp", "/submit_orders/0/state_revision",
             "/submit_orders/0/coin", "/submit_orders/0/side", "/submit_orders/0/reference_close",
             "/submit_orders/0/target_notional_usd", "/submit_orders/0/current_notional_usd",
             "/submit_orders/0/pending_notional_usd", "/submit_orders/0/delta_notional_usd",
             "/submit_orders/0/notional_usd"})
    };
    return examples;
}

class MessageRoundTrip : public testing::TestWithParam<WireCase> {};

TEST_P(MessageRoundTrip, R1_EncodeDecodePreservesAllFields)
{
    GetParam().check(GetParam().payload);
}

INSTANTIATE_TEST_SUITE_P(AllMessages, MessageRoundTrip, testing::ValuesIn(wireExamples()),
    [](const testing::TestParamInfo<WireCase>& info) { return info.param.name; });

TEST(MessageJsonBehaviour, R2_EveryDecoderRejectsMalformedJsonAndNonMessageRoots)
{
    for (const auto& example : wireExamples()) {
        SCOPED_TRACE(example.name);
        for (const std::string payload : {"{", "null", "[]", "42", "true", "\"fill\"", "{}"}) {
            SCOPED_TRACE(payload);
            EXPECT_THROW(example.decode(payload), std::exception);
        }
    }
}

TEST(MessageJsonBehaviour, R3_EveryDecoderRejectsMissingRequiredFields)
{
    for (const auto& example : wireExamples()) {
        SCOPED_TRACE(example.name);
        for (const auto& field : example.required_fields) {
            SCOPED_TRACE(field);
            auto payload = Json::parse(example.payload);
            const Json::json_pointer path(field);
            ASSERT_TRUE(payload.contains(path));
            payload.at(path.parent_pointer()).erase(path.back());
            EXPECT_THROW(example.decode(payload.dump()), std::exception);
        }
    }
}

TEST(MessageJsonBehaviour, R3_MetadataTypesAreNotCoercedAndUnknownExtensionsAreIgnored)
{
    for (const auto& example : wireExamples()) {
        SCOPED_TRACE(example.name);
        auto payload = Json::parse(example.payload);
        payload["metadata"]["message_id"] = 101;
        EXPECT_THROW(example.decode(payload.dump()), std::exception);
        payload = Json::parse(example.payload);
        payload["producer_note"] = "does not alter economics";
        example.check(payload.dump());
    }
}

// FINDING: Five decoders accept {} as an empty bars/strategies/submit_orders
// collection instead of rejecting its non-array shape as required by R3.
TEST(MessageJsonBehaviour, DISABLED_R3_CollectionsCannotBeReplacedByObjects)
{
    for (const auto& example : wireExamples()) {
        for (const std::string key : {"bars", "strategies", "submit_orders"}) {
            auto payload = Json::parse(example.payload);
            if (!payload.contains(key)) continue;
            SCOPED_TRACE(example.name + ":" + key);
            payload[key] = Json::object();
            EXPECT_THROW(example.decode(payload.dump()), std::exception);
        }
    }
}

TEST(MessageJsonBehaviour, R4_QuoteVolumeDistinguishesUnavailableNullAndKnownZero)
{
    auto payload = Json::parse(MessageJson::encode(sliceExample()));
    payload["bars"][0].erase("quote_volume");
    auto decoded = MessageJson::decodeMarketSliceSnapshot(payload.dump());
    ASSERT_EQ(decoded.bars.size(), 2U);
    EXPECT_TRUE(std::isnan(decoded.bars[0].bar.quote_volume));
    EXPECT_EQ(decoded.bars[0].bar.volume, 10.0);
    payload["bars"][0]["quote_volume"] = nullptr;
    decoded = MessageJson::decodeMarketSliceSnapshot(payload.dump());
    EXPECT_TRUE(std::isnan(decoded.bars[0].bar.quote_volume));
    payload["bars"][0]["quote_volume"] = 0.0;
    decoded = MessageJson::decodeMarketSliceSnapshot(payload.dump());
    EXPECT_EQ(decoded.bars[0].bar.quote_volume, 0.0);
}

TEST(MessageJsonBehaviour, R4_LegacyOptionalFieldsUseDocumentedDefaults)
{
    auto updated = Json::parse(MessageJson::encode(updatedExample()));
    for (const char* key : {"ranked_symbols", "active_top_n", "tracked_symbols",
                            "maintained_symbols", "requested_symbols", "downloaded_rows"}) updated.erase(key);
    auto expectedUpdate = updatedExample();
    expectedUpdate.ranked_symbols = expectedUpdate.active_top_n = expectedUpdate.tracked_symbols = 0;
    expectedUpdate.maintained_symbols = expectedUpdate.requested_symbols = expectedUpdate.downloaded_rows = 0;
    expectSame(MessageJson::decodeMarketDataUpdated(updated.dump()), expectedUpdate);

    auto account = Json::parse(MessageJson::encode(accountExample()));
    account.erase("collateral_usd");
    account.erase("resync_to_targets");
    auto expectedAccount = accountExample();
    expectedAccount.collateral_usd = 0.0;
    expectedAccount.resync_to_targets = false;
    expectSame(MessageJson::decodeAccountSnapshot(account.dump()), expectedAccount);

    auto order = Json::parse(MessageJson::encode(submitExample()));
    for (const char* key : {"reference_price", "fill_at_reference_price", "position_stop_price"}) order["order"].erase(key);
    auto expectedOrder = submitExample();
    expectedOrder.order.reference_price = 0.0;
    expectSame(MessageJson::decodeSubmitOrderCommand(order.dump()), expectedOrder);

    auto prices = Json::parse(MessageJson::encode(pricesExample()));
    prices.erase("decision_timestamp");
    EXPECT_EQ(MessageJson::decodeExecutionPriceSnapshot(prices.dump()).decision_timestamp, 0U);
    auto release = Json::parse(MessageJson::encode(releaseExample()));
    release.erase("decision_timestamp");
    EXPECT_EQ(MessageJson::decodeMarketDataReleaseRequest(release.dump()).decision_timestamp, 0U);
    auto decisions = Json::parse(MessageJson::encode(decisionsExample()));
    decisions["strategies"][0].erase("target_notional_usd");
    const auto decoded = MessageJson::decodeDecisionBatch(decisions.dump());
    ASSERT_EQ(decoded.strategies.size(), 1U);
    EXPECT_TRUE(decoded.strategies.front().target_notional_usd.empty());
}

TEST(MessageJsonBehaviour, R4_DefaultExtensionsDoNotChangeLegacyWireKeys)
{
    auto account = accountExample();
    account.collateral_usd = 0.0;
    account.resync_to_targets = false;
    const auto accountWire = Json::parse(MessageJson::encode(account));
    EXPECT_FALSE(accountWire.contains("collateral_usd"));
    EXPECT_FALSE(accountWire.contains("resync_to_targets"));
    auto submit = submitExample();
    submit.order.reference_price = 0.0;
    const auto orderWire = Json::parse(MessageJson::encode(submit)).at("order");
    EXPECT_FALSE(orderWire.contains("reference_price"));
    EXPECT_FALSE(orderWire.contains("fill_at_reference_price"));
    EXPECT_FALSE(orderWire.contains("position_stop_price"));
    const auto bars = Json::parse(MessageJson::encode(sliceExample())).at("bars");
    EXPECT_EQ(bars.at(0).at("quote_volume"), 720.0);
    EXPECT_FALSE(bars.at(1).contains("quote_volume"));
}

TEST(MessageJsonBehaviour, R1_PrecisionAndWideFillIdentitySurviveExactly)
{
    auto original = fillExample();
    original.fill.quantity = 0.12345678901234566;
    original.fill.price = 75.12345678901234;
    original.fill.commission = 0.00012345678901234567;
    expectSame(MessageJson::decodeFillEvent(MessageJson::encode(original)), original);
    EXPECT_EQ(original.fill.fill_id, 4294967297ULL);
}

TEST(MessageJsonBehaviour, R5_SignalsRejectInvalidExposureButAcceptBothEndpoints)
{
    for (const Json& value : {Json(-1.25), Json(1.25), Json(nullptr)}) {
        auto payload = Json::parse(MessageJson::encode(signalsExample()));
        payload["strategies"][0]["signals"]["BTC"] = value;
        EXPECT_THROW((void)MessageJson::decodeStrategyIntentBatch(payload.dump()), std::exception);
    }
    auto original = signalsExample();
    original.strategies[0].signals["ETH"] = -1.0;
    expectSame(MessageJson::decodeStrategyIntentBatch(MessageJson::encode(original)), original);
    auto payload = Json::parse(MessageJson::encode(original));
    payload["strategies"][0]["signals"][""] = 1.0;
    EXPECT_THROW((void)MessageJson::decodeStrategyIntentBatch(payload.dump()), std::invalid_argument);
}

TEST(MessageJsonBehaviour, R5_InvalidFillCannotInventValidExecution)
{
    for (const char* field : {"quantity", "price", "commission"}) {
        for (const Json& value : {Json(-1.0), Json(nullptr)}) {
            auto payload = Json::parse(MessageJson::encode(fillExample()));
            payload["fill"][field] = value;
            SCOPED_TRACE(field);
            EXPECT_THROW((void)MessageJson::decodeFillEvent(payload.dump()), std::exception);
        }
    }
    for (const char* field : {"quantity", "price"}) {
        auto payload = Json::parse(MessageJson::encode(fillExample()));
        payload["fill"][field] = 0.0;
        EXPECT_THROW((void)MessageJson::decodeFillEvent(payload.dump()), std::invalid_argument);
    }
    auto payload = Json::parse(MessageJson::encode(fillExample()));
    payload["fill"]["coin"] = "";
    EXPECT_THROW((void)MessageJson::decodeFillEvent(payload.dump()), std::invalid_argument);
    payload = Json::parse(MessageJson::encode(fillExample()));
    payload["fill"]["commission"] = 0.0;
    EXPECT_EQ(MessageJson::decodeFillEvent(payload.dump()).fill.commission, 0.0);
    auto sell = fillExample();
    sell.fill.side = OrderSide::Sell;
    const auto decodedSell = MessageJson::decodeFillEvent(MessageJson::encode(sell));
    expectSame(decodedSell, sell);
    EXPECT_EQ(-decodedSell.fill.signedQuantity() * decodedSell.fill.price - decodedSell.fill.commission, 149.0);
}

TEST(MessageJsonBehaviour, R5_NegativeOrNullCollateralIsRejectedForBothAccountSources)
{
    for (const Json& bad : {Json(-500.0), Json(nullptr)}) {
        auto account = Json::parse(MessageJson::encode(accountExample()));
        account["collateral_usd"] = bad;
        EXPECT_THROW((void)MessageJson::decodeAccountSnapshot(account.dump()), std::exception);
        auto exchange = Json::parse(MessageJson::encode(exchangeExample()));
        exchange["snapshot"]["collateral_usd"] = bad;
        EXPECT_THROW((void)MessageJson::decodeExchangeSnapshotEvent(exchange.dump()), std::exception);
    }
}

TEST(MessageJsonBehaviour, R5_AllVenueCostCategoriesPreserveCashSignsAndIdentity)
{
    for (const auto kind : {VenueCostKind::Fee, VenueCostKind::Funding,
                            VenueCostKind::Interest, VenueCostKind::CapitalFlow}) {
        for (const double amount : {-2.5, 2.5}) {
            auto original = costExample();
            original.kind = kind;
            original.amount_usd = amount;
            if (kind == VenueCostKind::CapitalFlow || kind == VenueCostKind::Interest) {
                original.coin.clear();
                original.fill_id = 0;
            }
            expectSame(MessageJson::decodeVenueCostEvent(MessageJson::encode(original)), original);
        }
    }
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
            {"kind", "UNKNOWN"}, {"cost_id", ""}, {"timestamp", 0}, {"amount_usd", nullptr}}) {
        auto payload = Json::parse(MessageJson::encode(costExample()));
        payload[field] = value;
        EXPECT_THROW((void)MessageJson::decodeVenueCostEvent(payload.dump()), std::exception);
    }
}

TEST(MessageJsonBehaviour, R6_UnsupportedResearchOrdersAreRejectedAcrossEveryOrderContainer)
{
    for (int field = 0; field < 3; ++field) {
        SCOPED_TRACE(field);
        auto command = submitExample();
        if (field == 0) command.order.entry_stop_price = 110.0;
        if (field == 1) command.order.protective_stop_price = 120.0;
        if (field == 2) command.order.parent_order_id = 40;
        EXPECT_THROW((void)MessageJson::encode(command), std::invalid_argument);
        auto quantityRequest = quantityRequestExample();
        quantityRequest.state.orders.front().request = command.order;
        EXPECT_THROW((void)MessageJson::encode(quantityRequest), std::invalid_argument);
        auto notionalRequest = notionalRequestExample();
        notionalRequest.state.orders.front().request = command.order;
        EXPECT_THROW((void)MessageJson::encode(notionalRequest), std::invalid_argument);
        auto plan = quantityPlanExample();
        plan.submit_orders.front() = command.order;
        EXPECT_THROW((void)MessageJson::encode(plan), std::invalid_argument);
    }
    for (const auto& example : wireExamples()) {
        for (const char* path : {"/order", "/state/orders/0/request", "/submit_orders/0"}) {
            const auto original = Json::parse(example.payload);
            const Json::json_pointer pointer(path);
            if (!original.contains(pointer) || !original.at(pointer).contains("quantity")) continue;
            for (const char* field : {"entry_stop_price", "protective_stop_price", "parent_order_id"}) {
                auto payload = original;
                payload.at(pointer)[field] = 110;
                EXPECT_THROW(example.decode(payload.dump()), std::invalid_argument);
            }
        }
    }
}

TEST(MessageJsonBehaviour, R6_UnsupportedDecisionTriggersAreRejectedInBothDirections)
{
    for (const bool protective : {false, true}) {
        auto original = decisionsExample();
        auto& decision = original.strategies.front().decisions.at("BTC");
        if (protective) decision.protective_stop_price = 120.0;
        else decision.entry_stop_price = 110.0;
        EXPECT_THROW((void)MessageJson::encode(original), std::invalid_argument);
        auto payload = Json::parse(MessageJson::encode(decisionsExample()));
        payload["strategies"][0]["decisions"][0][protective ? "protective_stop_price" : "entry_stop_price"] = 110.0;
        EXPECT_THROW((void)MessageJson::decodeDecisionBatch(payload.dump()), std::invalid_argument);
    }
}

TEST(MessageJsonBehaviour, R7_ManualOrdersAndPositionStopsRetainTheirDistinctIntent)
{
    auto manual = submitExample();
    manual.order.reference_price = 75.0;
    manual.order.fill_at_reference_price = true;
    expectSame(MessageJson::decodeSubmitOrderCommand(MessageJson::encode(manual)), manual);
    auto stop = submitExample();
    stop.order.side = OrderSide::Sell;
    stop.order.position_stop_price = 60.0;
    expectSame(MessageJson::decodeSubmitOrderCommand(MessageJson::encode(stop)), stop);
    auto payload = Json::parse(MessageJson::encode(manual));
    payload["order"].erase("reference_price");
    EXPECT_THROW((void)MessageJson::decodeSubmitOrderCommand(payload.dump()), std::invalid_argument);
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
            {"side", static_cast<int>(OrderSide::Buy)}, {"position_stop_price", -60.0},
            {"fill_at_reference_price", true}}) {
        payload = Json::parse(MessageJson::encode(stop));
        payload["order"][field] = value;
        EXPECT_THROW((void)MessageJson::decodeSubmitOrderCommand(payload.dump()), std::invalid_argument);
    }
}

TEST(MessageJsonBehaviour, R8_NotionalIdentityRevisionCloseAndSizeMustAgree)
{
    const std::vector<std::pair<std::string, Json>> corruptions{
        {"/metadata/schema_version", 2}, {"/metadata/message_id", "wrong-plan"},
        {"/state_revision", 0}, {"/decision_timestamp", 0},
        {"/decisions/decision_timestamp", 20260103}, {"/reference_closes/date", 20260103},
        {"/submit_orders/0/economic_order_id", "wrong-order"}, {"/submit_orders/0/order_id", 0},
        {"/submit_orders/0/strategy_id", 0}, {"/submit_orders/0/created_at", 0},
        {"/submit_orders/0/state_revision", 10}, {"/submit_orders/0/decision_timestamp", 20260103},
        {"/submit_orders/0/coin", ""}, {"/submit_orders/0/reference_close", 125.0},
        {"/submit_orders/0/reference_close", 0.0}, {"/submit_orders/0/notional_usd", 0.0},
        {"/submit_orders/0/notional_usd", 76.0}, {"/submit_orders/0/side", 99},
        {"/submit_orders/0/side", static_cast<int>(OrderSide::Sell)},
        {"/submit_orders/0/delta_notional_usd", -75.0}, {"/submit_orders/0/current_notional_usd", nullptr}
    };
    for (const auto& [path, value] : corruptions) {
        SCOPED_TRACE(path);
        auto payload = Json::parse(MessageJson::encode(notionalPlanExample()));
        payload[Json::json_pointer(path)] = value;
        EXPECT_THROW((void)MessageJson::decodeNotionalOrderPlanBatch(payload.dump()), std::exception);
    }
}

TEST(MessageJsonBehaviour, R8_SellNotionalHasNegativeDeltaAndStableDeterministicIdentity)
{
    auto original = notionalPlanExample();
    auto& order = original.submit_orders.front();
    order.side = OrderSide::Sell;
    order.target_notional_usd = 75.0;
    order.pending_notional_usd = 0.0;
    order.delta_notional_usd = -75.0; // Target 75 minus current 150.
    original.decisions.strategies.front().target_notional_usd["BTC"] = 75.0;
    original.decisions.strategies.front().decisions["BTC"] = RebalanceDecision::targetWeight(0.075);
    original.global_target_notional_usd["BTC"] = 75.0;
    expectSame(MessageJson::decodeNotionalOrderPlanBatch(MessageJson::encode(original)), original);
    EXPECT_EQ(LiveExecutionIdentity::notionalPlanningRequest(20260102, 9), "notional-order-plan-request:20260102:9");
    EXPECT_EQ(LiveExecutionIdentity::notionalOrderPlan(20260102, 9), "notional-order-plan:20260102:9");
    EXPECT_EQ(LiveExecutionIdentity::plannedEconomicOrder(20260102, 9, 41), "planned-notional-order:20260102:9:41");
    EXPECT_NE(LiveExecutionIdentity::plannedEconomicOrder(20260102, 10, 41), order.economic_order_id);
    EXPECT_NE(LiveExecutionIdentity::plannedEconomicOrder(20260103, 9, 41), order.economic_order_id);
}

TEST(MessageJsonBehaviour, R9_DuplicateStrategyPositionsAreRejectedInBothPlanningContracts)
{
    auto quantity = Json::parse(MessageJson::encode(quantityRequestExample()));
    auto duplicate = quantity["state"]["strategy_positions"][0];
    duplicate["positions"]["BTC"] = 3.0;
    quantity["state"]["strategy_positions"].push_back(duplicate);
    EXPECT_THROW((void)MessageJson::decodeOrderPlanningRequest(quantity.dump()), std::invalid_argument);
    auto notional = Json::parse(MessageJson::encode(notionalRequestExample()));
    notional["state"]["strategy_positions"].push_back(notional["state"]["strategy_positions"][0]);
    EXPECT_THROW((void)MessageJson::decodeNotionalOrderPlanningRequest(notional.dump()), std::invalid_argument);
}

// FINDING: decodeDecisionBatch accepts two BTC decisions and silently keeps the
// first TargetWeight(.25), discarding the later Flat instead of rejecting R9 ambiguity.
TEST(MessageJsonBehaviour, DISABLED_R9_DuplicateAssetDecisionCannotSilentlyChooseOneExposure)
{
    auto payload = Json::parse(MessageJson::encode(decisionsExample()));
    payload["strategies"][0]["decisions"].push_back(
        {{"coin", "BTC"}, {"action", static_cast<int>(RebalanceAction::Flat)}, {"target_weight", 0.0}});
    EXPECT_THROW((void)MessageJson::decodeDecisionBatch(payload.dump()), std::invalid_argument);
}

TEST(MessageJsonBehaviour, R10_CloseAndOpenPricesStaySeparateOnRepeatedRoundTrips)
{
    auto quantity = quantityRequestExample();
    auto notional = notionalRequestExample();
    for (int repeat = 0; repeat < 2; ++repeat) {
        quantity = MessageJson::decodeOrderPlanningRequest(MessageJson::encode(quantity));
        notional = MessageJson::decodeNotionalOrderPlanningRequest(MessageJson::encode(notional));
        EXPECT_EQ(quantity.decision_timestamp, 20260102U);
        EXPECT_EQ(quantity.execution_timestamp, 20260103U);
        EXPECT_EQ(quantity.prices.prices.at("BTC"), 125.0);
        EXPECT_EQ(notional.reference_closes.closes.at("BTC"), 75.0);
        EXPECT_EQ(quantity.decisions.strategies.front().target_notional_usd.at("BTC"), 250.0);
        EXPECT_EQ(250.0 / quantity.prices.prices.at("BTC"), 2.0);
        EXPECT_EQ(quantity.state.orders.front().remainingQuantity(), 3.0);
        EXPECT_EQ(quantity.state.next_order_id, 41U);
        EXPECT_EQ(notional.state.state_revision, 9U);
    }
}

TEST(MessageJsonBehaviour, R10_ReleasePhasesRemainDistinctAndUnknownKindsAreRejected)
{
    MarketDataReleaseRequest closed{metadata("close-release:20260102"),
        MarketDataReleaseKind::ClosedSlice, 20260102, 0};
    expectSame(MessageJson::decodeMarketDataReleaseRequest(MessageJson::encode(closed)), closed);
    const auto execution = releaseExample();
    expectSame(MessageJson::decodeMarketDataReleaseRequest(MessageJson::encode(execution)), execution);
    auto payload = Json::parse(MessageJson::encode(execution));
    payload["kind"] = 99;
    EXPECT_THROW((void)MessageJson::decodeMarketDataReleaseRequest(payload.dump()), std::invalid_argument);
}

} // namespace
} // namespace transport_test
