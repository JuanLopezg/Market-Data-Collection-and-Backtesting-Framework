#include "backtester.h"
#include "xh_breakout_strategy.h"
#include "timed_research_strategies.h"
#include "alphabetical_ranker.h"
#include "equal_weight_sizer.h"
#include "entry_exit_only_rebalance_policy.h"
#include "execution_engine.h"
#include "simulated_exchange.h"
#include "trading_state_snapshot.h"
#include "message_json.h"
#include "sqlite_state_store.h"
#include "trading_engine.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<class Action> void rejects(Action action)
{
    bool rejected = false;
    try { action(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Unsupported or invalid stop entry was accepted");
}

ExecutionOrder stopOrder()
{
    ExecutionOrder order(1, 1, 20200101, 20200102, "BTC", OrderSide::Buy, 10);
    order.entry_stop_price = 100;
    return order;
}

StrategyPortfolio portfolio(double atrMultiplier = 0)
{
    StrategyPortfolio result;
    result.emplace_back(1, std::make_unique<XHBreakoutStrategy>(
        1, std::make_unique<AllUniverseSelector>(), std::make_unique<AlphabeticalRanker>(),
        1, 2, atrMultiplier > 0 ? 1 : 2, atrMultiplier),
        1.0, std::make_unique<EqualWeightSizer>(0.1), RiskConstraints(1.5, 1.5),
        std::make_unique<EntryExitOnlyRebalancePolicy>());
    return result;
}

class MarketOnlyExchange final : public Exchange {
public:
    void submitOrder(const ExecutionOrder&) override { throw std::runtime_error("Unexpected dispatch"); }
    void cancelOrder(OrderID) override { throw std::runtime_error("Unexpected cancel"); }
    std::vector<ExchangeEvent> drainEvents() override { return {}; }
};

void shortBracketFixture(bool bullishEntry, bool timeExit)
{
    SimulatedExchange exchange(0.001);
    Account account(10000);
    TradeRecorder recorder;
    ExecutionEngine execution(std::vector<ExecutionStrategyDescriptor>{{1, "MRShort", {}}},
                              account, recorder, exchange);
    const auto persist = [](const std::optional<Fill>&) {};
    DecisionBatch decisions;
    decisions.decision_timestamp = 20200101;
    StrategyDecisionIntent intent;
    intent.strategy_id = 1;
    intent.decision_timestamp = 20200101;
    intent.reference_capital = 10000;
    auto entry = RebalanceDecision::stopEntry(-0.1, 90);
    entry.protective_stop_price = 110;
    intent.decisions["BTC"] = entry;
    decisions.strategies = {intent};
    ExecutionReferencePrices open;
    open.set("BTC", 80);
    execution.executeDecisionBatch(20200102, open, decisions, persist);
    execution.processExchangeEvents(persist);
    require(std::abs(execution.orderManager().find(1)->request.quantity - 12.5) < 1e-12,
            "Short gap quantity was not sized at the executable open");
    const CoinBarMap bars{{"BTC", {80, 115, 75, bullishEntry ? 85.0 : 78.0, 1000}}};
    exchange.processOpen(20200102, bars);
    require(exchange.drainEvents().empty(), "Short stop or cover matched as market entry");
    exchange.configureSplitNextFill(0.4);
    exchange.processStopEntriesAtClose(20200102, bars);
    const auto entryEvents = exchange.drainEvents();
    for (const auto& event : entryEvents)
        execution.processExchangeEvent(event, persist);
    for (const auto& event : entryEvents)
        execution.processExchangeEvent(event, persist);
    execution.processExchangeEvents(persist);
    require(execution.orderManager().orders().size() == 2 && execution.nextOrderId() == 3,
            "Split or repeated entry created duplicate protective covers");
    require(std::abs(account.cash() - 10999) < 1e-10 &&
            execution.strategyPosition(1).get("BTC") == -12.5,
            "Short sign, gap notional or fee incorrect");
    const auto child = execution.orderManager().find(2);
    require(child && child->request.quantity == 12.5 && child->pendingSignedQuantity() == 0.0,
            "Protective cover was not sized from actual fills or changed directional target");
    TradingStateSnapshot unsupportedSnapshot;
    unsupportedSnapshot.orders = {*child};
    rejects([&] { unsupportedSnapshot.requireMarketOnly(); });
    SubmitOrderCommand wire;
    wire.order = child->request;
    rejects([&] { MessageJson::encode(wire); });
    exchange.processProtectiveStopsAtClose(20200102, bars);
    execution.processExchangeEvents(persist);
    if (bullishEntry) {
        require(account.positions().get("BTC") == 0 && recorder.closedTrades().size() == 1,
                "Bullish entry-bar protective cover did not close the actual campaign");
        require(std::abs(account.cash() - 9622.625) < 1e-10,
                "Same-bar short PnL or two-sided fees incorrect");
        exchange.processProtectiveStopsAtClose(20200102, bars);
        require(exchange.drainEvents().empty(), "Repeated protective matching duplicated cover");
        return;
    }
    require(account.positions().get("BTC") == -12.5, "Bearish entry-bar high covered before entry");
    if (timeExit) {
        decisions.decision_timestamp = 20200102;
        intent.decision_timestamp = 20200102;
        intent.decisions["BTC"] = RebalanceDecision::flat();
        decisions.strategies = {intent};
        open.set("BTC", 85);
        execution.executeDecisionBatch(20200103, open, decisions, persist);
        execution.processExchangeEvents(persist);
        require(execution.orderManager().find(2)->status == ExecutionOrderStatus::Canceled,
                "Timed exit left its protective cover active");
        const CoinBarMap exitBars{{"BTC", {85, 120, 80, 90, 1000}}};
        exchange.processOpen(20200103, exitBars);
        execution.processExchangeEvents(persist);
        exchange.processProtectiveStopsAtClose(20200103, exitBars);
        require(exchange.drainEvents().empty() && account.positions().get("BTC") == 0,
                "Timed market cover and bracket both filled");
        require(std::abs(account.cash() - 9935.4375) < 1e-10, "Timed cover PnL or fees incorrect");
        return;
    }
    // A live/durable schema cannot store these fields; an explicit in-memory
    // snapshot retains the parent linkage and cover lifecycle without new events.
    SimulatedExchange resumedExchange(0.001);
    resumedExchange.restoreState(exchange.activeOrders(), exchange.nextFillId());
    Account resumedAccount(10000);
    // Preserve analytics alongside operational state for this in-memory proof.
    // Durable recovery would need a fill-log rebuild; that path stays unsupported.
    TradeRecorder resumedRecorder = recorder;
    ExecutionEngine resumed(std::vector<ExecutionStrategyDescriptor>{{1, "MRShort", {}}},
                            resumedAccount, resumedRecorder, resumedExchange);
    std::vector<TrackedOrder> orders;
    for (const auto& [id, order] : execution.orderManager().orders()) {
        (void)id;
        orders.push_back(order);
    }
    resumed.restoreState(account.cash(), account.positions().values(), execution.strategyPositions(),
        orders, execution.orderManager().processedFillIds(), execution.nextOrderId(), execution.lastExecutionTimestamp());
    const CoinBarMap later{{"BTC", {120, 125, 110, 115, 1000}}};
    exchange.processProtectiveStopsAtClose(20200103, later);
    execution.processExchangeEvents(persist);
    resumedExchange.processProtectiveStopsAtClose(20200103, later);
    resumed.processExchangeEvents(persist);
    require(account.positions().get("BTC") == 0 && resumedAccount.positions().get("BTC") == 0 &&
            std::abs(account.cash() - 9497.5) < 1e-10 && account.cash() == resumedAccount.cash(),
            "Restored protective gap fill changed account results");
    require(recorder.closedTrades().size() == 1 && resumedRecorder.closedTrades().size() == 1 &&
            recorder.closedTrades().front().pnl == resumedRecorder.closedTrades().front().pnl,
            "In-memory protective restoration lost short campaign analytics");
}

class TimedFixtureStrategy final : public TimedResearchStrategy {
public:
    explicit TimedFixtureStrategy(bool excludeEntryExit)
        : TimedResearchStrategy("Timed fixture", 1, std::make_unique<AllUniverseSelector>(),
            std::make_unique<AlphabeticalRanker>(), 1, 1, 1.0, excludeEntryExit) {}
private:
    bool qualifies(const Coin&, const BarData&, const MarketData&, Timestamp,
                   const IndicatorEngine&) const override { return true; }
};

void holdingPeriodFixture()
{
    const MarketData market{
        {20200101, {{"BTC", {100, 101, 99, 100, 1000}}}},
        {20200102, {{"ETH", {100, 101, 99, 100, 1000}}}},
        {20200103, {{"BTC", {100, 101, 99, 100, 1000}}}}
    };
    IndicatorEngine indicators;
    TradeRecord trade;
    trade.coin = "BTC";
    trade.start = 20200101;
    trade.direction = Direction::Long;
    for (bool excludeEntryExit : {false, true}) {
        TimedFixtureStrategy strategy(excludeEntryExit);
        strategy.observeTrades({trade});
        SignalState signals;
        strategy.updateSignals(market, 20200101, signals, indicators);
        require(signals.get("BTC") == (excludeEntryExit ? 1.0 : 0.0),
                "Market and stop holding periods lost their entry-bar distinction");
        strategy.updateSignals(market, 20200102, signals, indicators);
        require(signals.get("BTC") == 1.0, "Missing asset bar caused a timed exit");
        strategy.updateSignals(market, 20200103, signals, indicators);
        require(signals.get("BTC") == 0.0, "Observed holding period failed to exit");
    }
}

void missingShortEntryFixture()
{
    SimulatedExchange exchange;
    Account account(10000);
    TradeRecorder recorder;
    ExecutionEngine execution(std::vector<ExecutionStrategyDescriptor>{{1, "MRShort", {}}},
                              account, recorder, exchange);
    DecisionBatch decisions;
    decisions.decision_timestamp = 20200101;
    StrategyDecisionIntent intent;
    intent.strategy_id = 1;
    intent.decision_timestamp = 20200101;
    intent.reference_capital = 10000;
    auto entry = RebalanceDecision::stopEntry(-0.1, 90);
    entry.protective_stop_price = 110;
    intent.decisions["BTC"] = entry;
    decisions.strategies = {intent};
    const auto persist = [](const std::optional<Fill>&) {};
    execution.executeDecisionBatch(20200102, {}, decisions, persist);
    execution.processExchangeEvents(persist);
    exchange.processStopEntriesAtClose(20200102, {});
    execution.processExchangeEvents(persist);
    require(execution.orderManager().find(1)->status == ExecutionOrderStatus::Canceled &&
            execution.orderManager().orders().size() == 1 && account.cash() == 10000,
            "Missing short asset did not expire without filling or creating a cover");
}
}

int main()
{
    const auto order = stopOrder();
    SimulatedExchange exchange(0.001);
    Account account(10000);
    TradeRecorder recorder;
    ExecutionEngine execution(std::vector<ExecutionStrategyDescriptor>{{1, "XH", {}}},
                              account, recorder, exchange);
    OrderPlanBatch plan;
    plan.execution_timestamp = 20200102;
    plan.next_order_id = 2;
    plan.submit_orders = {order};
    const auto noPersistence = [](const std::optional<Fill>&) {};
    execution.applyOrderPlan(plan, noPersistence);
    execution.processExchangeEvents(noPersistence);
    require(execution.orderManager().find(1)->updated_at == 20200102,
            "Acceptance timestamp preceded submission");
    const CoinBarMap gap{{"BTC", {120, 125, 115, 123, 1000}}};
    exchange.processOpen(20200102, gap);
    require(exchange.drainEvents().empty(), "Stop matched using future high at open");
    exchange.configureSplitNextFill(0.4);
    exchange.processStopEntriesAtClose(20200102, gap);
    const auto events = exchange.drainEvents();
    require(events.size() == 3, "Split stop fill lost lifecycle event");
    for (const auto& event : events)
        execution.processExchangeEvent(event, noPersistence);
    const auto& tracked = *execution.orderManager().find(1);
    require(tracked.status == ExecutionOrderStatus::Canceled && tracked.pendingSignedQuantity() == 0,
            "Gap quantity cap left an executable remainder");
    require(std::abs(tracked.filled_quantity * 120 - 1000) < 1e-10,
            "Gap entry exceeded intended monetary amount");
    require(std::abs(account.cash() - 8999) < 1e-10, "Stop fees or cash incorrect");
    for (const auto& event : events)
        execution.processExchangeEvent(event, noPersistence);
    require(std::abs(account.cash() - 8999) < 1e-10, "Redelivered split fills duplicated money");
    exchange.processStopEntriesAtClose(20200102, gap);
    require(exchange.drainEvents().empty(), "Repeated close duplicated stop fill");

    for (const CoinBarMap bars : {CoinBarMap{{"BTC", {90, 99, 85, 95, 1000}}}, CoinBarMap{}}) {
        SimulatedExchange expired;
        expired.submitOrder(order);
        expired.drainEvents();
        expired.processStopEntriesAtClose(20200102, bars);
        const auto updates = expired.drainEvents();
        require(updates.size() == 1 && std::get<OrderUpdate>(updates.front()).status == ExecutionOrderStatus::Canceled,
                "Untriggered or missing-asset stop did not expire");
        expired.processStopEntriesAtClose(20200103, gap);
        require(expired.drainEvents().empty(), "Expired entry survived to a later bar");
    }
    SimulatedExchange pending;
    pending.submitOrder(order);
    pending.drainEvents();
    SimulatedExchange restored;
    restored.restoreState(pending.activeOrders(), pending.nextFillId());
    require(restored.drainEvents().empty(), "Restore recreated acceptance");
    const CoinBarMap crossing{{"BTC", {90, 105, 85, 102, 1000}}};
    pending.processStopEntriesAtClose(20200102, crossing);
    restored.processStopEntriesAtClose(20200102, crossing);
    const auto original = pending.drainEvents();
    const auto resumed = restored.drainEvents();
    const auto& fill = std::get<Fill>(original.front());
    const auto& restoredFill = std::get<Fill>(resumed.front());
    require(fill.price == 100 && fill.quantity == 10 && fill.timestamp == 20200102,
            "Crossed trigger filled at wrong price, size or time");
    require(fill.fill_id == restoredFill.fill_id && fill.price == restoredFill.price &&
            fill.quantity == restoredFill.quantity, "In-memory stop restore diverged");
    SimulatedExchange canceled;
    canceled.submitOrder(order);
    canceled.cancelOrderAt(1, 20200102);
    canceled.drainEvents();
    canceled.processStopEntriesAtClose(20200102, crossing);
    require(canceled.drainEvents().empty(), "Canceled stop filled");
    auto invalid = order;
    invalid.entry_stop_price = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { canceled.submitOrder(invalid); });
    rejects([&] { execution.applyOrderPlan(plan, noPersistence); });
    MarketOnlyExchange unsupported;
    Account untouched(10000);
    TradeRecorder untouchedRecorder;
    ExecutionEngine unsupportedExecution(std::vector<ExecutionStrategyDescriptor>{{1, "XH", {}}},
                                         untouched, untouchedRecorder, unsupported);
    rejects([&] { unsupportedExecution.applyOrderPlan(plan, noPersistence); });
    require(unsupportedExecution.nextOrderId() == 1 && !unsupportedExecution.orderManager().find(1),
            "Unsupported plan mutated state before rejection");
    TradingStateSnapshot snapshot;
    snapshot.orders.emplace_back(order);
    rejects([&] { snapshot.requireMarketOnly(); });
    SubmitOrderCommand command;
    command.order = order;
    rejects([&] { MessageJson::encode(command); });
    command.order.entry_stop_price = 0;
    auto wire = MessageJson::encode(command);
    const auto quantityField = wire.find("\"quantity\":");
    require(quantityField != std::string::npos, "Market order wire contract missing quantity");
    wire.insert(quantityField, "\"entry_stop_price\":100,");
    rejects([&] { MessageJson::decodeSubmitOrderCommand(wire); });
    SQLiteStateStore store(":memory:");
    TradingStateSnapshot marketSnapshot;
    marketSnapshot.account_cash = 10000;
    store.save(marketSnapshot);
    rejects([&] { store.save(snapshot); });
    require(store.load()->orders.empty() && store.load()->account_cash == 10000,
            "Rejected stop snapshot overwrote durable market state");
    snapshot.orders.clear();
    PendingPlanSnapshot pendingStop;
    pendingStop.plan.set("BTC", RebalanceDecision::stopEntry(0.1, 100));
    snapshot.pending_plans.push_back(pendingStop);
    rejects([&] { store.save(snapshot); });

    OHLCVData prices;
    prices.data["BTC"] = {
        {20200101, {100, 100, 99, 100, 1000}},
        {20200102, {100, 100, 99, 100, 1000}},
        {20200103, {90, 105, 89, 102, 1000}},
        {20200104, {102, 103, 89, 90, 1000}},
        {20200105, {80, 85, 75, 80, 1000}}
    };
    BacktestContext context(prices, portfolio(), 10000, 0.001);
    Backtester tester(context);
    tester.loop();
    const auto campaigns = context.GetTradeRecorder().closedTrades();
    require(campaigns.size() == 1, "XH duplicated or missed campaign");
    const auto& campaign = campaigns.front();
    require(campaign.start == 20200103 && campaign.end == 20200105 &&
            campaign.entry_price == 100 && campaign.exit_price == 80,
            "XH stop entry or deferred next-open exit incorrect");
    require(campaign.peak_quantity == 10, "Planner sized stop at following open instead of trigger");
    require(std::abs(context.GetCurrentEquity() - 9798.2) < 1e-8,
            "XH realized PnL or commissions incorrect");
    prices.data["BTC"].erase(20200105);
    BacktestContext prefix(prices, portfolio(), 10000, 0.001);
    Backtester prefixTester(prefix);
    prefixTester.loop();
    require(prefix.GetTradeRecorder().closedTrades().empty(), "Study cutoff forced an exit");
    require(prefix.GetAccount().positions().get("BTC") == 10, "Cutoff lost actual held quantity");
    for (std::size_t i = 0; i < prefix.GetAccountHistory().size(); ++i)
        require(prefix.GetAccountHistory()[i].equity == context.GetAccountHistory()[i].equity,
                "Future bars changed earlier XH equity");

    // A large entry wick must not become a trailing high. A later ATR increase
    // must not lower a stop already established by a quieter completed bar.
    prices.data["BTC"] = {
        {20200101, {100, 100, 99, 100, 1000}},
        {20200102, {100, 100, 99, 100, 1000}},
        {20200103, {90, 200, 89, 100, 1000}},
        {20200104, {105, 110, 104, 109, 1000}},
        {20200105, {109, 120, 80, 99, 1000}},
        {20200106, {90, 100, 85, 95, 1000}}
    };
    BacktestContext atr(prices, portfolio(1), 10000);
    Backtester atrTester(atr);
    atrTester.loop();
    const auto atrCampaigns = atr.GetTradeRecorder().closedTrades();
    require(atrCampaigns.size() == 1 && atrCampaigns.front().end == 20200106,
            "ATR included entry wick, lowered its ratchet or exited at signal close");
    TradeRecord held = atrCampaigns.front();
    held.exited = false;
    auto rebuilt = portfolio(1);
    rebuilt.front().strategy().observeTrades({held});
    SignalState signals;
    rebuilt.front().strategy().updateSignals(atr.GetMarketData(), 20200104, signals, atr.GetIndicatorEngine());
    require(signals.get("BTC") == 1, "Rebuilt trailing state included entry wick");
    rebuilt.front().strategy().updateSignals(atr.GetMarketData(), 20200105, signals, atr.GetIndicatorEngine());
    require(signals.get("BTC") == 0, "Rebuilt trailing ratchet lost prior stop");
    SimulatedExchange researchExchange;
    Account researchAccount(10000);
    TradeRecorder researchRecorder;
    TradingEngine researchEngine(rebuilt, researchAccount, researchRecorder,
                                 atr.GetIndicatorEngine(), researchExchange);
    bool attachmentRejected = false;
    try { researchEngine.attachStateStore(store); }
    catch (const std::logic_error&) { attachmentRejected = true; }
    require(attachmentRejected, "Research stop strategy admitted to unsupported durable recovery");
    shortBracketFixture(true, false);
    shortBracketFixture(false, false);
    shortBracketFixture(false, true);
    holdingPeriodFixture();
    missingShortEntryFixture();
    std::cout << "RESEARCH-STOP: PASS: long/short matching, brackets, holding periods, gaps, expiry, "
                 "split-fill idempotency, in-memory restore, boundary rejection, fees, cutoff, causal ATR ratchet\n";
}
