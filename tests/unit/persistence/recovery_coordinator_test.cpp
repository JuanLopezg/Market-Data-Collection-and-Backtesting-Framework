#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "fake_exchange.h"
#include "recovery_coordinator.h"
#include "snapshot_examples.h"
#include "sqlite_backend.h"
#include "validated/pure_rsi.h"

namespace persistence_test {
namespace {

StrategyPortfolio recoveryStrategies()
{
    StrategyPortfolio strategies;
    for (const StrategyID id : {7U, 9U}) {
        strategies.emplace_back(id,
            std::make_unique<StrategyPureRSI>(1, nullptr, nullptr, 1, 7, 50.0, 40.0),
            0.5, std::make_unique<EqualWeightSizer>(0.25), RiskConstraints(1.0, 1.0),
            std::make_unique<EntryExitOnlyRebalancePolicy>());
    }
    return strategies;
}

class RecoveryBehaviour : public SQLiteBackend {
protected:
    StrategyPortfolio strategies_ = recoveryStrategies();
    Account account_{1000.0};
    TradeRecorder recorder_;
    IndicatorEngine indicators_;
    FakeExchange venue_{1000.0};
    TradingEngine engine_{strategies_, account_, recorder_, indicators_, venue_};
    RecoveryCoordinator coordinator_;

    static ExchangeSnapshot matchingExample()
    {
        ExchangeSnapshot exchange;
        exchange.timestamp = 20260102;
        exchange.cash = 849.0;
        exchange.positions = {{"BTC", 2.0}, {"ETH", -0.5}};
        exchange.open_orders.push_back({41, "venue-41", "BTC", OrderSide::Buy, 5.0, 2.0});
        return exchange;
    }

    void expectNoRecoveryCommands() const
    {
        EXPECT_EQ(venue_.submitCount(), 0U);
        EXPECT_EQ(venue_.cancelRequestCount(41), 0U);
    }
};

TEST_F(RecoveryBehaviour, R15_CleanFreshStartupCreatesFirstCheckpoint)
{
    ASSERT_FALSE(store_->load().has_value());
    const auto result = coordinator_.recover(engine_, *store_, venue_.snapshot(20260101));
    EXPECT_FALSE(result.restored_persisted_state);
    EXPECT_TRUE(result.ready_for_trading);
    EXPECT_TRUE(result.reconciliation.clean());
    EXPECT_TRUE(engine_.tradingEnabled());
    EXPECT_EQ(account_.cash(), 1000.0);
    EXPECT_TRUE(account_.positions().values().empty());
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->account_cash, 1000.0);
    EXPECT_EQ(loaded->next_order_id, 1U);
    EXPECT_TRUE(loaded->orders.empty());
    EXPECT_TRUE(store_->loadFills().empty());
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R15_DriftOnFreshStartupLeavesStoreAbsentAndTradingPaused)
{
    auto exchange = venue_.snapshot(20260101);
    exchange.positions["BTC"] = 2.0;
    const auto result = coordinator_.recover(engine_, *store_, exchange);
    EXPECT_FALSE(result.restored_persisted_state);
    EXPECT_FALSE(result.ready_for_trading);
    EXPECT_FALSE(engine_.tradingEnabled());
    ASSERT_EQ(result.reconciliation.issues.size(), 1U);
    EXPECT_EQ(result.reconciliation.issues.front().kind, ReconciliationIssueKind::PositionMismatch);
    EXPECT_FALSE(store_->load().has_value());
    EXPECT_THROW(engine_.onBarClose(MarketData{}, 20260102, PriceSnapshot{}), std::logic_error);
    EXPECT_THROW(engine_.executePendingPlans(20260103, PriceSnapshot{}), std::logic_error);
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R15_RestoreKeepsPendingCloseIntentAndPartialOrderWithoutSending)
{
    const auto snapshot = exampleSnapshot();
    store_->save(snapshot, exampleFill());
    reopen();
    const auto result = coordinator_.recover(engine_, *store_, matchingExample());
    EXPECT_TRUE(result.restored_persisted_state);
    EXPECT_TRUE(result.ready_for_trading);
    EXPECT_TRUE(result.reconciliation.clean());
    EXPECT_TRUE(engine_.tradingEnabled());
    expectSnapshot(engine_.stateSnapshot(), snapshot);
    EXPECT_TRUE(engine_.hasPendingPlans());
    EXPECT_EQ(account_.cash(), 849.0);
    EXPECT_EQ(account_.positions().get("BTC"), 2.0);
    EXPECT_EQ(account_.positions().get("ETH"), -0.5);
    const auto* order = engine_.orderManager().find(41);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remainingQuantity(), 3.0);
    EXPECT_TRUE(order->cancel_requested);
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R15_DriftKeepsPersistedTruthPausedUntilCleanRetry)
{
    const auto snapshot = exampleSnapshot();
    store_->save(snapshot, exampleFill());
    auto exchange = matchingExample();
    exchange.cash = 848.0;
    const auto blocked = coordinator_.recover(engine_, *store_, exchange);
    EXPECT_TRUE(blocked.restored_persisted_state);
    EXPECT_FALSE(blocked.ready_for_trading);
    EXPECT_FALSE(engine_.tradingEnabled());
    ASSERT_EQ(blocked.reconciliation.issues.size(), 1U);
    EXPECT_EQ(blocked.reconciliation.issues.front().kind, ReconciliationIssueKind::CashMismatch);
    expectSnapshot(engine_.stateSnapshot(), snapshot);
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    EXPECT_THROW(engine_.onBarClose(MarketData{}, 20260103, PriceSnapshot{}), std::logic_error);
    EXPECT_THROW(engine_.executePendingPlans(20260103, PriceSnapshot{}), std::logic_error);
    const auto clean = coordinator_.recover(engine_, *store_, matchingExample());
    EXPECT_TRUE(clean.ready_for_trading);
    EXPECT_TRUE(engine_.tradingEnabled());
    EXPECT_EQ(store_->loadFills().size(), 1U);
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R16_RebuildChronologicalTradePnlWithoutReapplyingCash)
{
    TradingStateSnapshot flat;
    flat.account_cash = 1048.0;
    flat.last_bar_close_timestamp = 20260103;
    flat.last_execution_timestamp = 20260103;
    flat.next_order_id = 43;
    flat.processed_fill_ids = {101, 102};
    auto sell = exampleFill(102);
    sell.order_id = 42;
    sell.timestamp = 20260103;
    sell.side = OrderSide::Sell;
    sell.price = 100.0;
    // Deliberately write later economics first: analytics must use event time.
    store_->save(flat, sell);
    store_->save(flat, exampleFill());
    reopen();
    ExchangeSnapshot exchange;
    exchange.timestamp = 20260103;
    exchange.cash = 1048.0;
    for (int restart = 0; restart < 2; ++restart) {
        SCOPED_TRACE(restart);
        const auto result = coordinator_.recover(engine_, *store_, exchange);
        EXPECT_TRUE(result.ready_for_trading);
        EXPECT_EQ(account_.cash(), 1048.0); // 1000 - 150 - 1 + 200 - 1.
        EXPECT_TRUE(account_.positions().values().empty());
        ASSERT_EQ(recorder_.closedTrades().size(), 1U);
        const auto& trade = recorder_.closedTrades().front();
        EXPECT_EQ(trade.start, 20260102U);
        EXPECT_EQ(trade.end, 20260103U);
        EXPECT_EQ(trade.strategy_id, 7U);
        EXPECT_EQ(trade.coin, "BTC");
        EXPECT_EQ(trade.peak_quantity, 2.0);
        EXPECT_EQ(trade.entry_price, 75.0);
        EXPECT_EQ(trade.exit_price, 100.0);
        EXPECT_EQ(trade.commission, 2.0);
        EXPECT_EQ(trade.pnl, 48.0); // 2*(100 - 75) - (1 + 1).
        EXPECT_EQ(recorder_.cumulativeClosedPnl(), 48.0);
        EXPECT_EQ(store_->loadFills().size(), 2U);
    }
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R16_RecoveredProcessedFillIdPreventsEconomicReplay)
{
    // Build the external venue's actual state, retaining its queued event delivery.
    ExecutionOrder request(41, 7, 20260101, 20260102, "BTC", OrderSide::Buy, 2.0);
    FakeExchange filledVenue(1000.0, 1.0 / 150.0); // Fee = 2*75/150 = 1.
    filledVenue.submitOrder(request);
    filledVenue.acceptOrder(41, 20260102);
    const auto fill = filledVenue.fillOrder(41, 2.0, 75.0, 20260102);
    TradingEngine recovering(strategies_, account_, recorder_, indicators_, filledVenue);
    TradingStateSnapshot snapshot;
    snapshot.account_cash = 849.0;
    snapshot.account_positions = {{"BTC", 2.0}};
    snapshot.next_order_id = 42;
    snapshot.last_execution_timestamp = 20260102;
    snapshot.strategies.push_back({7, {{"BTC", 1.0}}, {{"BTC", 0.25}}, {{"BTC", 2.0}}});
    TrackedOrder order(request);
    order.status = ExecutionOrderStatus::Filled;
    order.filled_quantity = 2.0;
    order.updated_at = 20260102;
    snapshot.orders.push_back(order);
    snapshot.processed_fill_ids = {fill.fill_id};
    store_->save(snapshot, fill);
    const auto result = coordinator_.recover(recovering, *store_, filledVenue.snapshot(20260102));
    ASSERT_TRUE(result.ready_for_trading);
    filledVenue.replayLastFill();
    recovering.processExchangeEvents();
    EXPECT_EQ(account_.cash(), 849.0);
    EXPECT_EQ(account_.positions().get("BTC"), 2.0);
    const auto* restored = recovering.orderManager().find(41);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->filled_quantity, 2.0);
    EXPECT_EQ(restored->status, ExecutionOrderStatus::Filled);
    PriceSnapshot marks;
    marks.set("BTC", 75.0);
    const auto trades = recorder_.allTrades(marks, 20260102);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_EQ(trades.front().commission, 1.0);
    EXPECT_EQ(trades.front().pnl, -1.0);
    EXPECT_EQ(store_->loadFills().size(), 1U);
    EXPECT_EQ(filledVenue.submitCount(), 1U); // Recovery sent no additional order.
}

TEST_F(RecoveryBehaviour, R16_OrphanAuditFailsClosedWithoutInventingOperationalState)
{
    store_->save(exampleSnapshot(), exampleFill());
    executeSql("DELETE FROM runtime_state;");
    ASSERT_FALSE(store_->load().has_value());
    EXPECT_THROW((void)coordinator_.recover(engine_, *store_, venue_.snapshot(20260102)), std::runtime_error);
    EXPECT_FALSE(engine_.tradingEnabled());
    EXPECT_FALSE(store_->load().has_value());
    EXPECT_EQ(store_->loadFills().size(), 1U);
    EXPECT_EQ(account_.cash(), 1000.0);
    expectNoRecoveryCommands();
}

TEST_F(RecoveryBehaviour, R15_UnknownRecoveredStrategyKeepsTradingPaused)
{
    auto snapshot = exampleSnapshot();
    snapshot.strategies.front().strategy_id = 99;
    store_->save(snapshot);
    EXPECT_THROW((void)coordinator_.recover(engine_, *store_, matchingExample()), std::out_of_range);
    EXPECT_FALSE(engine_.tradingEnabled());
    const auto loaded = store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    expectNoRecoveryCommands();
}

} // namespace
} // namespace persistence_test
