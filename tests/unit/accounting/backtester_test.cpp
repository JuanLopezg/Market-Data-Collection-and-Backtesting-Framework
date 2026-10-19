#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include "backtest_test_support.h"

namespace {
using namespace accounting_test;

void expectSnapshot(const BacktestAccountSnapshot& snapshot, Timestamp date,
                    double cash, double balance, double equity)
{
    EXPECT_EQ(snapshot.timestamp, date);
    EXPECT_NEAR(snapshot.cash, cash, 1e-9);
    EXPECT_NEAR(snapshot.balance, balance, 1e-9);
    EXPECT_NEAR(snapshot.equity, equity, 1e-9);
}

TEST(Backtester, R17_LastBarSignalCannotFillAtItsOwnCloseOrEarlierOpen)
{
    BacktestContext context(market(1), oneStrategy(), 1000.0);
    Backtester backtester(context);
    backtester.loop();
    ASSERT_EQ(context.GetAccountHistory().size(), 1U);
    expectSnapshot(context.GetAccountHistory().front(), kD1, 1000.0, 1000.0, 1000.0);
    EXPECT_TRUE(context.GetAccount().positions().values().empty());
    EXPECT_TRUE(context.GetTradeRecorder().allTrades(marks(100.0), kD1).empty());
}

TEST(Backtester, R17_CloseDecisionFillsNextOpenAndExitUsesActualUnits)
{
    BacktestContext context(market(), oneStrategy(), 1000.0);
    Backtester backtester(context);
    backtester.loop();
    const auto& history = context.GetAccountHistory();
    ASSERT_EQ(history.size(), 3U);
    expectSnapshot(history[0], kD1, 1000.0, 1000.0, 1000.0);
    expectSnapshot(history[1], kD2, 500.0, 1000.0, 1100.0);
    expectSnapshot(history[2], kD3, 1060.0, 1060.0, 1060.0);
    EXPECT_DOUBLE_EQ(context.GetAccount().positions().get("BTC"), 0.0);
    ASSERT_EQ(context.GetTradeRecorder().closedTrades().size(), 1U);
    const auto& trade = context.GetTradeRecorder().closedTrades().front();
    EXPECT_EQ(trade.start, kD2);
    EXPECT_EQ(trade.end, kD3);
    EXPECT_DOUBLE_EQ(trade.entry_price, 125.0);
    EXPECT_DOUBLE_EQ(trade.exit_price, 140.0);
    EXPECT_DOUBLE_EQ(trade.peak_quantity, 4.0);
    EXPECT_DOUBLE_EQ(trade.pnl, 60.0);
}

TEST(Backtester, R17_FeesUseActualEntryAndExitNotionalExactlyOnce)
{
    BacktestContext context(market(), oneStrategy(), 1000.0, 0.01);
    Backtester backtester(context);
    backtester.loop();
    const auto& history = context.GetAccountHistory();
    ASSERT_EQ(history.size(), 3U);
    expectSnapshot(history[1], kD2, 495.0, 1000.0, 1095.0);
    expectSnapshot(history[2], kD3, 1049.4, 1049.4, 1049.4);
    ASSERT_EQ(context.GetTradeRecorder().closedTrades().size(), 1U);
    EXPECT_DOUBLE_EQ(context.GetTradeRecorder().closedTrades().front().commission, 10.6);
    EXPECT_NEAR(context.GetTradeRecorder().closedTrades().front().pnl, 49.4, 1e-9);
}

TEST(Backtester, R18_OpenGapChangesUnitsButPreservesFrozenUsdTarget)
{
    for (const double executionOpen : {50.0, 250.0}) {
        SCOPED_TRACE(executionOpen);
        auto data = market(2);
        data.data["BTC"][kD2] = bar(executionOpen, 150.0);
        BacktestContext context(data, oneStrategy(), 1000.0);
        Backtester backtester(context);
        backtester.loop();
        EXPECT_DOUBLE_EQ(context.GetCurrentCash(), 500.0);
        EXPECT_DOUBLE_EQ(context.GetAccount().positions().get("BTC"), executionOpen == 50.0 ? 10.0 : 2.0);
        EXPECT_DOUBLE_EQ(context.GetCurrentEquity(), executionOpen == 50.0 ? 2000.0 : 800.0);
        const auto trades = context.GetTradeRecorder().allTrades(marks(150.0), kD2);
        ASSERT_EQ(trades.size(), 1U);
        EXPECT_DOUBLE_EQ(trades.front().entry_price, executionOpen);
        EXPECT_FALSE(trades.front().exited);
    }
}

TEST(Backtester, R18_HoldKeepsFilledUnitsDespiteChangedCloseEquity)
{
    BacktestContext context(market(), oneStrategy({{kD1, 1.0}}), 1000.0);
    Backtester backtester(context);
    backtester.loop();
    EXPECT_DOUBLE_EQ(context.GetAccount().positions().get("BTC"), 4.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentCash(), 500.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentBalance(), 1000.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentEquity(), 1020.0);
    const auto trades = context.GetTradeRecorder().allTrades(marks(130.0), kD3);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_DOUBLE_EQ(trades.front().pnl, 20.0);
}

TEST(Backtester, R18_ShortSaleAndLaterCoverProduceCorrectSignedCashAndProfit)
{
    auto data = market();
    data.data["BTC"][kD3] = bar(100.0, 130.0);
    BacktestContext context(data, oneStrategy({{kD1, -1.0}, {kD2, 0.0}}), 1000.0);
    Backtester backtester(context);
    backtester.loop();
    ASSERT_EQ(context.GetAccountHistory().size(), 3U);
    expectSnapshot(context.GetAccountHistory()[1], kD2, 1500.0, 1000.0, 900.0);
    expectSnapshot(context.GetAccountHistory()[2], kD3, 1100.0, 1100.0, 1100.0);
    ASSERT_EQ(context.GetTradeRecorder().closedTrades().size(), 1U);
    const auto& trade = context.GetTradeRecorder().closedTrades().front();
    EXPECT_EQ(trade.direction, Direction::Short);
    EXPECT_DOUBLE_EQ(trade.entry_price, 125.0);
    EXPECT_DOUBLE_EQ(trade.exit_price, 100.0);
    EXPECT_DOUBLE_EQ(trade.pnl, 100.0);
}

TEST(Backtester, R18_OpposingStrategiesNetAccountExposureButRetainUnitsAndFees)
{
    StrategyPortfolio portfolio;
    portfolio.push_back(strategy(kAlpha, 0.5, 1.0, {{kD1, 1.0}}));
    portfolio.push_back(strategy(kBeta, 0.5, 1.0, {{kD1, -1.0}}));
    BacktestContext context(market(2), std::move(portfolio), 1000.0, 0.01);
    Backtester backtester(context);
    backtester.loop();
    EXPECT_DOUBLE_EQ(context.GetAccount().positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(context.strategyById(kAlpha).virtualPositions().get("BTC"), 4.0);
    EXPECT_DOUBLE_EQ(context.strategyById(kBeta).virtualPositions().get("BTC"), -4.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentCash(), 990.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentEquity(), 990.0);
    EXPECT_DOUBLE_EQ(context.GetCurrentBalance(), 1000.0);
    const auto trades = context.GetTradeRecorder().allTrades(marks(150.0), kD2);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_DOUBLE_EQ(trades[0].commission, 5.0);
    EXPECT_DOUBLE_EQ(trades[1].commission, 5.0);
    EXPECT_DOUBLE_EQ(trades[0].pnl + trades[1].pnl, -10.0);
    EXPECT_TRUE(context.GetTradeRecorder().closedTrades().empty());
}

TEST(Backtester, R19_CutoffAndCloseTradesDoNotManufactureExitFillsOrFees)
{
    BacktestContext context(market(2), oneStrategy(), 1000.0, 0.01);
    Backtester backtester(context);
    backtester.loop();
    backtester.closeTrades();
    backtester.closeTrades();
    ASSERT_EQ(context.GetAccountHistory().size(), 2U);
    expectSnapshot(context.GetAccountHistory().back(), kD2, 495.0, 1000.0, 1095.0);
    EXPECT_DOUBLE_EQ(context.GetAccount().positions().get("BTC"), 4.0);
    EXPECT_TRUE(context.GetTradeRecorder().closedTrades().empty());
    const auto trades = context.GetTradeRecorder().allTrades(marks(150.0), kD2);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_FALSE(trades.front().exited);
    EXPECT_DOUBLE_EQ(trades.front().commission, 5.0);
    EXPECT_DOUBLE_EQ(trades.front().pnl, 95.0);
    EXPECT_EQ(trades.front().fill_count, 1U);
}

TEST(Backtester, R19_FuturePricePerturbationCannotChangeEarlierSnapshots)
{
    BacktestContext baseline(market(), oneStrategy(), 1000.0, 0.01);
    auto changed = market();
    changed.data["BTC"][kD3] = bar(20000.0, 30000.0);
    changed.data["BTC"][kD3].high = 40000.0;
    changed.data["BTC"][kD3].low = 10000.0;
    BacktestContext perturbed(changed, oneStrategy(), 1000.0, 0.01);
    Backtester(baseline).loop();
    Backtester(perturbed).loop();
    ASSERT_EQ(baseline.GetAccountHistory().size(), 3U);
    ASSERT_EQ(perturbed.GetAccountHistory().size(), 3U);
    for (const auto* context : {&baseline, &perturbed}) {
        expectSnapshot(context->GetAccountHistory()[0], kD1, 1000.0, 1000.0, 1000.0);
        expectSnapshot(context->GetAccountHistory()[1], kD2, 495.0, 1000.0, 1095.0);
    }
}

TEST(Backtester, R19_ExtendingHistoryPreservesPreviouslyObservedCashBalanceAndEquity)
{
    BacktestContext prefix(market(2), oneStrategy(), 1000.0, 0.01);
    BacktestContext extended(market(), oneStrategy(), 1000.0, 0.01);
    Backtester(prefix).loop();
    Backtester(extended).loop();
    ASSERT_EQ(prefix.GetAccountHistory().size(), 2U);
    ASSERT_EQ(extended.GetAccountHistory().size(), 3U);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& earlier = prefix.GetAccountHistory()[i];
        const auto& later = extended.GetAccountHistory()[i];
        EXPECT_EQ(later.timestamp, earlier.timestamp);
        EXPECT_DOUBLE_EQ(later.cash, earlier.cash);
        EXPECT_DOUBLE_EQ(later.balance, earlier.balance);
        EXPECT_DOUBLE_EQ(later.equity, earlier.equity);
    }
    expectSnapshot(extended.GetAccountHistory()[2], kD3, 1049.4, 1049.4, 1049.4);
}

TEST(BacktestContext, R19_CompatibilityTradeViewsPreserveOpenAndClosedFillEconomics)
{
    BacktestContext openContext(market(2), oneStrategy(), 1000.0, 0.01);
    Backtester(openContext).loop();
    const auto& open = openContext.GetTradesHistory();
    ASSERT_EQ(open.size(), 1U);
    const auto openTrade = open.begin()->second;
    EXPECT_FALSE(openTrade.exited_);
    EXPECT_EQ(openTrade.start_, kD2);
    EXPECT_DOUBLE_EQ(openTrade.entry_, 125.0);
    EXPECT_DOUBLE_EQ(openTrade.exit_, 150.0);
    EXPECT_DOUBLE_EQ(openTrade.size_, 4.0);
    EXPECT_DOUBLE_EQ(openTrade.commission_, 5.0);
    EXPECT_DOUBLE_EQ(openTrade.pnl_, 95.0);
    EXPECT_DOUBLE_EQ(openContext.GetCurrentCash(), 495.0);

    BacktestContext closedContext(market(), oneStrategy(), 1000.0, 0.01);
    Backtester(closedContext).loop();
    const auto& closed = closedContext.GetTradesHistory();
    ASSERT_EQ(closed.size(), 1U);
    const auto& closedTrade = closed.begin()->second;
    EXPECT_TRUE(closedTrade.exited_);
    EXPECT_EQ(closedTrade.end_, kD3);
    EXPECT_DOUBLE_EQ(closedTrade.exit_, 140.0);
    EXPECT_DOUBLE_EQ(closedTrade.commission_, 10.6);
    EXPECT_NEAR(closedTrade.pnl_, 49.4, 1e-9);
    EXPECT_NEAR(closedContext.GetCurrentCash(), 1049.4, 1e-9);
}

TEST(BacktestContext, R20_InvalidCapitalCostsOrStrategyAllocationRejectSetup)
{
    const auto data = market(1);
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW({ BacktestContext context(data, oneStrategy(), bad); (void)context; }, std::invalid_argument);
    }
    for (const double bad : {-0.01, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW({ BacktestContext context(data, oneStrategy(), 1000.0, bad); (void)context; }, std::invalid_argument);
    }
    EXPECT_THROW({ BacktestContext context(data, StrategyPortfolio{}, 1000.0); (void)context; }, std::invalid_argument);
    StrategyPortfolio duplicate;
    duplicate.push_back(strategy(kAlpha, 0.5, 1.0, {}));
    duplicate.push_back(strategy(kAlpha, 0.5, 1.0, {}));
    EXPECT_THROW({ BacktestContext context(data, std::move(duplicate), 1000.0); (void)context; }, std::invalid_argument);
    StrategyPortfolio excessive;
    excessive.push_back(strategy(kAlpha, 0.6, 1.0, {}));
    excessive.push_back(strategy(kBeta, 0.6, 1.0, {}));
    EXPECT_THROW({ BacktestContext context(data, std::move(excessive), 1000.0); (void)context; }, std::invalid_argument);
}

TEST(BacktestContext, R16_PartialRealizationDoesNotEnterBalanceUntilCampaignCloses)
{
    BacktestContext context(market(), oneStrategy(), 1000.0);
    for (const auto& incoming : std::vector<Fill>{
             fill(101, OrderSide::Buy, 2.0, 100.0, 2.0),
             fill(102, OrderSide::Buy, 2.0, 200.0, 4.0, kD2),
             fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3)}) {
        context.GetAccount().applyFill(incoming);
        context.GetTradeRecorder().onFill(incoming, "Alpha");
    }
    context.recordAccountSnapshot(kD3, marks(170.0));
    ASSERT_EQ(context.GetAccountHistory().size(), 1U);
    const auto partial = context.GetAccountHistory().front();
    expectSnapshot(partial, kD3, 572.2, 1000.0, 1082.2);
    const auto closing = fill(104, OrderSide::Sell, 3.0, 140.0, 4.2, kD4);
    context.GetAccount().applyFill(closing);
    context.GetTradeRecorder().onFill(closing, "Alpha");
    context.recordAccountSnapshot(kD4, marks(140.0));
    ASSERT_EQ(context.GetAccountHistory().size(), 2U);
    expectSnapshot(context.GetAccountHistory()[1], kD4, 988.0, 988.0, 988.0);
    expectSnapshot(context.GetAccountHistory()[0], kD3, 572.2, 1000.0, 1082.2);
    expectSnapshot(partial, kD3, 572.2, 1000.0, 1082.2);
    ASSERT_EQ(context.GetBalanceEquityHistoric().size(), 2U);
    EXPECT_DOUBLE_EQ(context.GetBalanceEquityHistoric()[0].first, 1000.0);
    EXPECT_NEAR(context.GetBalanceEquityHistoric()[0].second, 1082.2, 1e-9);
}

} // namespace
