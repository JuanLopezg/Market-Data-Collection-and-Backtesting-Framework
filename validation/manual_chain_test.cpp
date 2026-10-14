// Manual Control in the trading chain: the pieces below the services.
//
// 1. The re-sync after a manual rebalance moves HOLD positions back to the risk-approved
//    weights and adds a strategy whose decisions were all HOLD.
// 2. The new wire fields are written only when set, so normal messages keep their bytes.
// 3. The simulated exchange fills one manual order at its price and nothing else.
// 4. Manual orders do not move the daily-cycle markers of the execution engine.
// 5. The request folder: parsing, the processed record, and the re-sync marker.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "account.h"
#include "adapters/simulated_exchange.h"
#include "execution_engine.h"
#include "manual_requests.h"
#include "message_json.h"
#include "target_decisions.h"
#include "trade_recorder.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void resyncMovesHoldPositionsBackToTargets()
{
    // Strategy 1 decided only ETH (BTC was a HOLD); strategy 2 only had HOLDs, so risk
    // omitted it. After a manual rebalance both must return to their approved weights.
    DecisionBatch approved;
    approved.decision_timestamp = 20261014;
    StrategyDecisionIntent first;
    first.strategy_id = 1;
    first.decision_timestamp = 20261014;
    first.reference_capital = 1000.0;
    first.decisions.emplace("ETHUSDT", RebalanceDecision::targetWeight(0.2));
    first.target_notional_usd.emplace("ETHUSDT", 200.0);
    approved.strategies.push_back(first);

    std::vector<PortfolioRiskEvaluation> evaluations(2);
    evaluations[0].strategy_id = 1;
    evaluations[0].sizing_available = true;
    evaluations[0].reference_capital = 1000.0;
    evaluations[0].approved_weights.set("BTCUSDT", 0.5);
    evaluations[0].approved_weights.set("ETHUSDT", 0.2);
    evaluations[1].strategy_id = 2;
    evaluations[1].sizing_available = true;
    evaluations[1].reference_capital = 500.0;
    evaluations[1].approved_weights.set("SOLUSDT", 0.4);

    StrategyPositionSnapshot positions;
    positions[1].set("BTCUSDT", 0.01);   // manual size, approved weight 50%
    positions[1].set("XRPUSDT", 100.0);  // bought manually, not a strategy target
    positions[2].set("SOLUSDT", 3.0);

    ExecutionReferencePrices prices;
    for (const auto& [coin, price] : {std::pair{"BTCUSDT", 60000.0}, {"ETHUSDT", 3000.0}, {"XRPUSDT", 0.5}, {"SOLUSDT", 150.0}})
        prices.set(coin, price);

    const DecisionBatch resync = resyncToApprovedTargets(approved, evaluations, positions, prices);
    check(resync.strategies.size() == 2, "the all-HOLD strategy is added");
    const auto& one = resync.strategies.at(0);
    check(one.target_notional_usd.at("BTCUSDT") == 500.0, "HOLD BTC moves to its approved weight");
    check(one.target_notional_usd.at("ETHUSDT") == 200.0, "ETH target kept");
    check(one.decisions.at("XRPUSDT").action == RebalanceAction::Flat, "manual-only asset is closed");
    const auto& two = resync.strategies.at(1);
    check(two.strategy_id == 2 && two.decision_timestamp == 20261014, "added strategy identity");
    check(two.target_notional_usd.at("SOLUSDT") == 200.0, "added strategy uses its own capital");
}

void wireFieldsOnlyWhenSet()
{
    AccountSnapshot snapshot;
    snapshot.metadata.schema_version = 1;
    snapshot.metadata.message_id = "account-snapshot:market-data:20261014";
    snapshot.timestamp = 20261014;
    snapshot.cash = 100.0;
    const std::string normal = MessageJson::encode(snapshot);
    check(normal.find("resync_to_targets") == std::string::npos, "normal snapshot keeps its bytes");
    check(!MessageJson::decodeAccountSnapshot(normal).resync_to_targets, "absent flag decodes false");
    snapshot.resync_to_targets = true;
    check(MessageJson::decodeAccountSnapshot(MessageJson::encode(snapshot)).resync_to_targets, "flag round trip");

    // Venue collateral (Kraken LIVE): only written when set, so PAPER snapshots keep their bytes.
    check(normal.find("collateral_usd") == std::string::npos, "no collateral field without collateral");
    snapshot.collateral_usd = 613.8;
    check(MessageJson::decodeAccountSnapshot(MessageJson::encode(snapshot)).collateral_usd == 613.8, "collateral round trip");
    ExchangeSnapshotEvent venue;
    venue.metadata.schema_version = 1;
    venue.metadata.message_id = "kraken-futures-snapshot:1";
    venue.snapshot.timestamp = 20261014;
    check(MessageJson::encode(venue).find("collateral_usd") == std::string::npos, "simulated exchange snapshot keeps its bytes");
    venue.snapshot.collateral_usd = 613.8;
    check(MessageJson::decodeExchangeSnapshotEvent(MessageJson::encode(venue)).snapshot.collateral_usd == 613.8,
          "venue collateral round trip");

    // New venue cash kinds travel by name.
    VenueCostEvent flow;
    flow.metadata.schema_version = 1;
    flow.metadata.message_id = "kraken-futures-cost:10:flow";
    flow.cost_id = "kraken-futures:10:flow";
    flow.kind = VenueCostKind::CapitalFlow;
    flow.timestamp = 20261014;
    flow.amount_usd = 50.0;
    const std::string flowJson = MessageJson::encode(flow);
    check(flowJson.find("\"CAPITAL_FLOW\"") != std::string::npos &&
          MessageJson::decodeVenueCostEvent(flowJson).kind == VenueCostKind::CapitalFlow, "capital flow round trip");

    SubmitOrderCommand command;
    command.metadata.schema_version = 1;
    command.metadata.message_id = "submit-order-7";
    command.order = ExecutionOrder(7, 1, 20261014, 20261014, "BTCUSDT", OrderSide::Buy, 0.5);
    const std::string plain = MessageJson::encode(command);
    check(plain.find("fill_at_reference_price") == std::string::npos, "daily order keeps its bytes");
    command.order.reference_price = 60000.0;
    command.order.fill_at_reference_price = true;
    const SubmitOrderCommand decoded = MessageJson::decodeSubmitOrderCommand(MessageJson::encode(command));
    check(decoded.order.fill_at_reference_price && decoded.order.reference_price == 60000.0, "manual order round trip");

    bool rejected = false;
    try {
        MessageJson::decodeSubmitOrderCommand(
            R"({"metadata":{"schema_version":1,"message_id":"m","correlation_id":"","produced_at":0},)"
            R"("order":{"order_id":1,"strategy_id":1,"created_at":1,"active_from":1,"coin":"BTCUSDT","side":0,)"
            R"("quantity":1,"fill_at_reference_price":true}})");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "fill at reference price without a price is refused");
}

void simulatedExchangeFillsOnlyTheManualOrder()
{
    SimulatedExchange exchange(0.001);
    exchange.submitOrder(ExecutionOrder(1, 1, 20261014, 20261015, "ETHUSDT", OrderSide::Buy, 2.0));
    exchange.submitOrder(ExecutionOrder(2, 1, 20261014, 20261014, "BTCUSDT", OrderSide::Buy, 0.5));
    exchange.drainEvents();
    exchange.fillAtPrice(2, 20261014, 60000.0);
    const auto events = exchange.drainEvents();
    check(events.size() == 2, "one fill and one terminal update");
    const auto* fill = std::get_if<Fill>(&events.at(0));
    check(fill != nullptr && fill->order_id == 2 && fill->price == 60000.0 && fill->quantity == 0.5,
          "manual order filled at its price");
    check(fill != nullptr && std::abs(fill->commission - 30.0) < 1e-9, "commission at the configured rate");
    check(exchange.activeOrders().size() == 1 && exchange.activeOrders().contains(1), "other orders untouched");
}

void manualOrdersKeepTheDailyCycleMarkers()
{
    Account account(1000.0);
    TradeRecorder recorder;
    SimulatedExchange exchange;
    ExecutionEngine engine(std::vector<ExecutionStrategyDescriptor>{{1, "Pure_RSI", {}}}, account, recorder, exchange);
    engine.restoreState(1000.0, {}, {}, {}, {}, 5, 20261014);

    ExecutionOrder order(5, 1, 20261014, 20261014, "BTCUSDT", OrderSide::Buy, 0.01);
    order.reference_price = 60000.0;
    order.fill_at_reference_price = true;
    int saves = 0;
    engine.applyManualOrders({order}, 6, 20261014, [&](const std::optional<Fill>&) { ++saves; });
    check(engine.lastExecutionTimestamp() == 20261014, "last execution timestamp unchanged");
    check(engine.nextOrderId() == 6, "order id advanced");
    check(saves == 2, "staged state saved before the submit and after it");
    check(exchange.activeOrders().contains(5), "order reached the exchange");

    bool rejected = false;
    try {
        engine.applyManualOrders({order}, 7, 20261014, [](const std::optional<Fill>&) {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "an order id is never sent twice");
}

void requestFolder()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("manual_chain_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "requests");
    fs::create_directories(root / "processed");
    const ManualRequestFolder folder(root);

    const auto write = [&](const std::string& name, const std::string& body) {
        std::ofstream(root / "requests" / name) << body;
        return root / "requests" / name;
    };
    const fs::path good = write("0002-manual-0123456789abcdef.request",
        "MANUAL_REQUEST_V1\nrequest_id=manual-0123456789abcdef\nactor=operator / OPERATOR\n"
        "request_hash=sha256:abc\ncreated_at=2026-10-14T12:00:00Z\ntarget=BTCUSDT,0.250000000000\n"
        "target=1000PEPEUSDT,0.100000000000\n");
    write("0001-manual-bad.request", "MANUAL_REQUEST_V1\nrequest_id=manual-bad\n");

    check(folder.nextRequest()->filename() == "0001-manual-bad.request", "oldest request first");
    bool invalid = false;
    try {
        folder.read(root / "requests" / "0001-manual-bad.request");
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid, "malformed request refused");

    const ManualRequest request = folder.read(good);
    check(request.weights.size() == 2 && request.weights.at("BTCUSDT") == 0.25 &&
          request.weights.at("1000PEPEUSDT") == 0.1, "targets parsed");

    ManualOutcome outcome;
    outcome.executed = true;
    outcome.detail = "2 orders\nsent";
    outcome.execution_day = 20261014;
    outcome.first_order = 40;
    outcome.last_order = 41;
    folder.finish(good, request.raw_lines, outcome);
    check(!fs::exists(good) && folder.alreadyProcessed(good), "request moved to processed");
    std::ifstream record(root / "processed" / good.filename());
    const std::string text((std::istreambuf_iterator<char>(record)), std::istreambuf_iterator<char>());
    check(text.find("result=EXECUTED\ndetail=2 orders sent\n") != std::string::npos, "outcome on one line");
    check(text.find("execution_day=20261014\norders=40-41\n") != std::string::npos, "orders recorded");

    check(!folder.resyncForDay(20261014), "no marker, no re-sync");
    folder.markResyncPending();
    check(folder.resyncForDay(20261014), "pending marker used by the next daily snapshot");
    check(folder.resyncForDay(20261014), "redelivered snapshot of that day keeps it");
    check(!folder.resyncForDay(20261015), "later days do not");
    fs::remove_all(root);
}

} // namespace

int main()
{
    resyncMovesHoldPositionsBackToTargets();
    wireFieldsOnlyWhenSet();
    simulatedExchangeFillsOnlyTheManualOrder();
    manualOrdersKeepTheDailyCycleMarkers();
    requestFolder();
    if (failures != 0) {
        std::cerr << failures << " checks failed\n";
        return 1;
    }
    std::cout << "manual chain test: all checks pass\n";
    return 0;
}
