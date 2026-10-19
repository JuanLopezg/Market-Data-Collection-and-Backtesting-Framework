#include <limits>

#include <gtest/gtest.h>

#include "fake_exchange.h"
#include "runtime_examples.h"

namespace runtime_test {
namespace {

TEST(TradingBehaviour, R5_CloseAndSubmitDoNotSpendCashBeforeTheOpenFill)
{
    TradingRig rig;
    rig.through(3);
    expectAccount(rig, 1000.0, 0.0);
    EXPECT_TRUE(rig.engine.orderManager().orders().empty());
    EXPECT_TRUE(rig.recorder.closedTrades().empty());
    rig.engine.executePendingPlans(openTime(4), prices(125.0));
    expectAccount(rig, 1000.0, 0.0);
    ASSERT_EQ(rig.engine.orderManager().orders().size(), 1U);
    const auto* order = rig.engine.orderManager().find(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->request.side, OrderSide::Buy);
    EXPECT_EQ(order->request.active_from, openTime(4));
    EXPECT_EQ(order->request.quantity, 2.0);
    EXPECT_TRUE(rig.engine.orderManager().processedFillIds().empty());
    rig.exchange.processOpen(openTime(4), rig.history.marketData().at(closeTime(4)));
    rig.engine.processExchangeEvents();
    expectAccount(rig, 750.0, 2.0);
}

TEST(TradingBehaviour, R5_FiveDayRoundTripClosesTwoUnitsForFiftyDollarsProfit)
{
    TradingRig rig;
    rig.through(4);
    expectAccount(rig, 750.0, 2.0);
    EXPECT_EQ(rig.account.equity(prices(100.0)), 950.0);
    ASSERT_TRUE(rig.engine.hasPendingPlans());
    rig.completeDay(5);
    expectAccount(rig, 1050.0, 0.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 1U);
    const auto& trade = rig.recorder.closedTrades().front();
    EXPECT_EQ(trade.start, openTime(4));
    EXPECT_EQ(trade.end, openTime(5));
    EXPECT_EQ(trade.entry_price, 125.0);
    EXPECT_EQ(trade.exit_price, 150.0);
    EXPECT_EQ(trade.peak_quantity, 2.0);
    EXPECT_EQ(trade.pnl, 50.0);
    EXPECT_EQ(rig.engine.orderManager().orders().size(), 2U);
    EXPECT_EQ(rig.engine.orderManager().processedFillIds().size(), 2U);
}

TEST(TradingBehaviour, R5_SameCloseExecutionCannotCreateAClosingPriceEntry)
{
    TradingRig rig;
    rig.through(3);
    EXPECT_THROW(rig.engine.executePendingPlans(closeTime(3), prices(120.0)), std::exception);
    expectAccount(rig, 1000.0, 0.0);
    EXPECT_TRUE(rig.exchange.activeOrders().empty());
    EXPECT_TRUE(rig.engine.orderManager().orders().empty());
}

TEST(TradingBehaviour, R6_CommissionsReduceCashAndClosedPnlByFivePointFive)
{
    TradingRig rig({StrategyConfig{}}, 0.01);
    rig.through(5);
    expectAccount(rig, 1044.5, 0.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 1U);
    EXPECT_EQ(rig.recorder.closedTrades().front().commission, 5.5);
    EXPECT_EQ(rig.recorder.closedTrades().front().pnl, 44.5);
    EXPECT_EQ(rig.recorder.cumulativeClosedPnl(), 44.5);
}

TEST(TradingBehaviour, R6_SplitEntryPreservesTheSameQuantityCashAndFees)
{
    TradingRig rig({StrategyConfig{}}, 0.01);
    rig.through(3);
    rig.exchange.configureSplitNextFill(0.5);
    rig.completeDay(4);
    expectAccount(rig, 747.5, 2.0);
    rig.completeDay(5);
    expectAccount(rig, 1044.5, 0.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 1U);
    EXPECT_EQ(rig.recorder.closedTrades().front().fill_count, 3U);
    EXPECT_EQ(rig.recorder.closedTrades().front().commission, 5.5);
    EXPECT_EQ(rig.recorder.closedTrades().front().pnl, 44.5);
}

TEST(TradingBehaviour, R3_EntryExitHoldDoesNotResizeOnAContinuingSignal)
{
    TradingRig rig({StrategyConfig{}}, 0.0, 130.0);
    rig.through(4);
    expectAccount(rig, 750.0, 2.0);
    EXPECT_EQ(rig.account.equity(prices(130.0)), 1010.0);
    EXPECT_FALSE(rig.engine.hasPendingPlans());
    EXPECT_EQ(rig.engine.orderManager().orders().size(), 1U);
}

TEST(TradingBehaviour, R2_ExtraCollateralChangesSizingButIsNeverCreditedAsCash)
{
    TradingRig rig;
    rig.close(1);
    rig.close(2);
    rig.close(3, 500.0);
    expectAccount(rig, 1000.0, 0.0);
    const auto snapshot = rig.engine.stateSnapshot();
    ASSERT_EQ(snapshot.pending_plans.size(), 1U);
    EXPECT_EQ(snapshot.pending_plans.front().plan.referenceCapital(), 1500.0);
    rig.open(4);
    expectAccount(rig, 625.0, 3.0);
    EXPECT_EQ(rig.account.equity(prices(125.0)), 1000.0);
}

TEST(TradingBehaviour, R2_InvalidExtraCapitalCannotCreateOrSpendAnOrder)
{
    for (const double invalid : {-1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        TradingRig rig;
        EXPECT_THROW(rig.close(3, invalid), std::invalid_argument);
        expectAccount(rig, 1000.0, 0.0);
        EXPECT_FALSE(rig.engine.hasPendingPlans());
        EXPECT_TRUE(rig.exchange.activeOrders().empty());
    }
}

TEST(TradingBehaviour, R7_RejectedEntryLeavesCashPositionsAndAnalyticsUnchanged)
{
    TradingRig rig;
    rig.through(3);
    rig.exchange.configureRejectNextOrder("Synthetic venue rejection");
    rig.open(4);
    expectAccount(rig, 1000.0, 0.0);
    const auto* order = rig.engine.orderManager().find(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status, ExecutionOrderStatus::Rejected);
    EXPECT_TRUE(rig.engine.orderManager().processedFillIds().empty());
    EXPECT_TRUE(rig.recorder.allTrades(prices(125.0), openTime(4)).empty());
}

TEST(TradingBehaviour, R7_RepeatingExecutionWhileAnEntryIsPendingCannotStackOrders)
{
    TradingRig rig;
    rig.through(3);
    rig.engine.executePendingPlans(openTime(4), prices(125.0));
    rig.engine.executePendingPlans(openTime(4), prices(125.0));
    ASSERT_EQ(rig.exchange.activeOrders().size(), 1U);
    EXPECT_EQ(rig.exchange.activeOrders().at(1).quantity, 2.0);
    EXPECT_EQ(rig.engine.stateSnapshot().next_order_id, 2U);
    rig.exchange.processOpen(openTime(4), rig.history.marketData().at(closeTime(4)));
    rig.engine.processExchangeEvents();
    expectAccount(rig, 750.0, 2.0);
    EXPECT_EQ(rig.engine.orderManager().processedFillIds().size(), 1U);
}

TEST(TradingBehaviour, R8_PauseBlocksNewWorkButStillAccountsForQueuedFills)
{
    TradingRig rig;
    rig.through(3);
    rig.engine.executePendingPlans(openTime(4), prices(125.0));
    rig.engine.pauseTrading();
    EXPECT_THROW(rig.close(4), std::logic_error);
    EXPECT_THROW(rig.engine.executePendingPlans(openTime(5), prices(150.0)), std::logic_error);
    rig.exchange.processOpen(openTime(4), rig.history.marketData().at(closeTime(4)));
    rig.engine.processExchangeEvents();
    expectAccount(rig, 750.0, 2.0);
    EXPECT_FALSE(rig.engine.tradingEnabled());
    rig.engine.resumeTrading();
    rig.close(4);
    rig.completeDay(5);
    expectAccount(rig, 1050.0, 0.0);
}

TEST(TradingBehaviour, R3_SplitEntryUniverseKeepsHeldMarkAndClosesActualUnits)
{
    TradingRig rig;
    rig.through(3);
    rig.open(4);
    MarketData entryUniverse;
    BarData eth;
    eth.open = eth.high = eth.low = eth.close = 100.0;
    entryUniverse[closeTime(4)].emplace("ETHUSDT", eth);
    rig.engine.onBarClose(entryUniverse, rig.history.marketData(), closeTime(4), prices(100.0), 0.0);
    const auto snapshot = rig.engine.stateSnapshot();
    ASSERT_EQ(snapshot.pending_plans.size(), 1U);
    EXPECT_EQ(snapshot.pending_plans.front().plan.referenceCapital(), 950.0);
    EXPECT_EQ(snapshot.pending_plans.front().plan.get("BTCUSDT").action, RebalanceAction::Flat);
    rig.completeDay(5);
    expectAccount(rig, 1050.0, 0.0);
}

TEST(TradingBehaviour, R3_RestoredShortIsCoveredByABuyAndBooksPositivePnl)
{
    TradingRig rig;
    Fill opening;
    opening.fill_id = 101;
    opening.order_id = 99;
    opening.strategy_id = 7;
    opening.timestamp = openTime(3);
    opening.coin = "BTCUSDT";
    opening.side = OrderSide::Sell;
    opening.quantity = 2.0;
    opening.price = 125.0;
    TradingStateSnapshot snapshot;
    snapshot.account_cash = 1250.0;
    snapshot.account_positions = {{"BTCUSDT", -2.0}};
    snapshot.strategies.push_back({7, {}, {}, {{"BTCUSDT", -2.0}}});
    snapshot.next_order_id = 100;
    snapshot.processed_fill_ids = {101};
    TrackedOrder order(ExecutionOrder(99, 7, closeTime(2), openTime(3), "BTCUSDT", OrderSide::Sell, 2.0));
    order.status = ExecutionOrderStatus::Filled;
    order.filled_quantity = 2.0;
    snapshot.orders.push_back(order);
    rig.engine.restoreState(snapshot);
    rig.engine.rebuildTradeRecorder({opening});
    rig.close(4);
    rig.engine.executePendingPlans(openTime(5), prices(100.0));
    CoinBarMap bars;
    BarData bar;
    bar.open = bar.high = bar.low = bar.close = 100.0;
    bars.emplace("BTCUSDT", bar);
    rig.exchange.processOpen(openTime(5), bars);
    rig.engine.processExchangeEvents();
    expectAccount(rig, 1050.0, 0.0);
    const auto* cover = rig.engine.orderManager().find(100);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->request.side, OrderSide::Buy);
    EXPECT_EQ(cover->request.quantity, 2.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 1U);
    EXPECT_EQ(rig.recorder.closedTrades().front().pnl, 50.0);
}

TEST(TradingBehaviour, R3_MultipleStrategiesAttributeFillsWithoutDuplicatingTheAccount)
{
    TradingRig rig({StrategyConfig{7, 0.5}, StrategyConfig{9, 0.5}});
    rig.through(4);
    expectAccount(rig, 750.0, 2.0);
    EXPECT_EQ(rig.strategies[0].virtualPositions().get("BTCUSDT"), 1.0);
    EXPECT_EQ(rig.strategies[1].virtualPositions().get("BTCUSDT"), 1.0);
    rig.completeDay(5);
    expectAccount(rig, 1050.0, 0.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 2U);
    for (const auto& trade : rig.recorder.closedTrades()) EXPECT_EQ(trade.pnl, 25.0);
    EXPECT_EQ(rig.recorder.cumulativeClosedPnl(), 50.0);
}

TEST(TradingBehaviour, R9_AttachingAStoreDoesNotOverwriteUnrestoredMoney)
{
    test_support::TempDir directory("runtime-attach");
    SQLiteStateStore store((directory / "state.sqlite").string());
    TradingStateSnapshot persisted;
    persisted.account_cash = 750.0;
    persisted.account_positions = {{"BTCUSDT", 2.0}};
    store.save(persisted);
    TradingRig rig;
    rig.engine.attachStateStore(store);
    const auto saved = store.load();
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->account_cash, 750.0);
    EXPECT_EQ(saved->account_positions.at("BTCUSDT"), 2.0);
    expectAccount(rig, 1000.0, 0.0);
}

TEST(TradingBehaviour, R9_CompletedDayRestartReproducesCashPnlAndDurableFills)
{
    for (const unsigned boundary : {3U, 4U}) {
        SCOPED_TRACE(boundary);
        test_support::TempDir directory("runtime-restart");
        SQLiteStateStore store((directory / "state.sqlite").string());
        TradingRig continuous;
        continuous.through(5);
        OrderID nextFill = 1;
        TradingStateSnapshot saved;
        std::unordered_map<OrderID, ExecutionOrder> venueOrders;
        {
            TradingRig interrupted;
            interrupted.engine.attachStateStore(store);
            interrupted.through(boundary);
            interrupted.engine.checkpoint();
            const auto loaded = store.load();
            ASSERT_TRUE(loaded.has_value());
            saved = *loaded;
            venueOrders = interrupted.exchange.activeOrders();
            nextFill = static_cast<OrderID>(interrupted.exchange.nextFillId());
            EXPECT_EQ(store.loadFills().size(), boundary == 3 ? 0U : 1U);
        }
        TradingRig resumed;
        resumed.exchange.restoreState(venueOrders, nextFill);
        ExchangeSnapshot exchange;
        exchange.timestamp = closeTime(boundary);
        exchange.cash = saved.account_cash;
        exchange.positions = saved.account_positions;
        RecoveryCoordinator recovery;
        const auto result = recovery.recover(resumed.engine, store, exchange);
        ASSERT_TRUE(result.ready_for_trading);
        EXPECT_TRUE(resumed.exchange.activeOrders().empty());
        for (unsigned day = boundary + 1; day <= 5; ++day) resumed.completeDay(day);
        expectAccount(resumed, 1050.0, 0.0);
        expectTradingStatesEqual(resumed.engine.stateSnapshot(), continuous.engine.stateSnapshot());
        ASSERT_EQ(resumed.recorder.closedTrades().size(), 1U);
        EXPECT_EQ(resumed.recorder.closedTrades().front().pnl, 50.0);
        const auto fills = store.loadFills();
        ASSERT_EQ(fills.size(), 2U);
        EXPECT_EQ(fills[0].fill_id, 1U);
        EXPECT_EQ(fills[0].quantity, 2.0);
        EXPECT_EQ(fills[0].price, 125.0);
        EXPECT_EQ(fills[1].fill_id, 2U);
        EXPECT_EQ(fills[1].side, OrderSide::Sell);
        EXPECT_EQ(fills[1].quantity, 2.0);
        EXPECT_EQ(fills[1].price, 150.0);
    }
}

TEST(TradingBehaviour, R9_InvalidRestoredStateCannotDispatchNewExchangeCommands)
{
    TradingRig source;
    source.through(4);
    for (int error = 0; error < 6; ++error) {
        SCOPED_TRACE(error);
        auto state = source.engine.stateSnapshot();
        if (error == 0) state.schema_version = 2;
        if (error == 1) state.next_order_id = 0;
        if (error == 2) state.next_order_id = 1;
        if (error == 3) state.strategies.front().strategy_id = 99;
        if (error == 4) state.strategies.push_back(state.strategies.front());
        if (error == 5) state.pending_plans.push_back(state.pending_plans.front());
        TradingRig restored;
        EXPECT_THROW(restored.engine.restoreState(state), std::exception);
        EXPECT_TRUE(restored.exchange.activeOrders().empty());
    }
}

struct AsyncRig {
    RollingMarketState history = marketHistory();
    IndicatorEngine indicators;
    StrategyPortfolio strategies = tradingStrategies({StrategyConfig{}});
    Account account{1000.0};
    TradeRecorder recorder;
    FakeExchange venue;
    TradingEngine engine;

    explicit AsyncRig(double fee = 0.0)
        : venue(1000.0, fee), engine(strategies, account, recorder, indicators, venue)
    {
        indicators.precompute(history.rawData(), {{IndicatorKind::RSI, PriceField::Close, 2}});
        for (unsigned day = 1; day <= 3; ++day)
            engine.onBarClose(history.marketData(), closeTime(day), history.closePrices(closeTime(day)));
        engine.executePendingPlans(openTime(4), prices(125.0));
        venue.acceptOrder(1, openTime(4));
        engine.processExchangeEvents();
    }
};

TEST(TradingAsyncBehaviour, R6_ReplayedPartialFillDoesNotChargeCashOrFeesTwice)
{
    AsyncRig rig(0.01);
    rig.venue.fillOrder(1, 1.0, 125.0, openTime(4));
    rig.engine.processExchangeEvents();
    EXPECT_EQ(rig.account.cash(), 873.75);
    EXPECT_EQ(rig.account.positions().get("BTCUSDT"), 1.0);
    rig.venue.replayLastFill();
    rig.engine.processExchangeEvents();
    EXPECT_EQ(rig.account.cash(), 873.75);
    EXPECT_EQ(rig.account.positions().get("BTCUSDT"), 1.0);
    const auto trades = rig.recorder.allTrades(prices(125.0), openTime(4));
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_EQ(trades.front().commission, 1.25);
    EXPECT_EQ(trades.front().pnl, -1.25);
    const auto* order = rig.engine.orderManager().find(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remainingQuantity(), 1.0);
}

TEST(TradingAsyncBehaviour, R7_CanceledRemainderKeepsActualPartialFillAndExitSize)
{
    AsyncRig rig;
    rig.venue.fillOrder(1, 1.0, 125.0, openTime(4));
    rig.engine.processExchangeEvents();
    rig.engine.orderManager().markCancelRequested(1, manualTime(4));
    rig.venue.cancelOrder(1);
    rig.venue.confirmCancel(1, manualTime(4));
    rig.engine.processExchangeEvents();
    EXPECT_EQ(rig.account.cash(), 875.0);
    EXPECT_EQ(rig.account.positions().get("BTCUSDT"), 1.0);
    rig.engine.onBarClose(rig.history.marketData(), closeTime(4), prices(100.0));
    rig.engine.executePendingPlans(openTime(5), prices(150.0));
    const auto* exit = rig.engine.orderManager().find(2);
    ASSERT_NE(exit, nullptr);
    EXPECT_EQ(exit->request.side, OrderSide::Sell);
    EXPECT_EQ(exit->request.quantity, 1.0);
    rig.venue.acceptOrder(2, openTime(5));
    rig.venue.fillOrder(2, 1.0, 150.0, openTime(5));
    rig.engine.processExchangeEvents();
    EXPECT_EQ(rig.account.cash(), 1025.0);
    EXPECT_EQ(rig.account.positions().get("BTCUSDT"), 0.0);
    ASSERT_EQ(rig.recorder.closedTrades().size(), 1U);
    EXPECT_EQ(rig.recorder.closedTrades().front().pnl, 25.0);
}

} // namespace
} // namespace runtime_test
