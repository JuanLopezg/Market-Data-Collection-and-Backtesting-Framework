#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

#include "accounting_test_support.h"

namespace {
using namespace accounting_test;

TEST(TradeRecorder, R13_PartialReductionKeepsOriginalEntryAndWholeCampaignOpen)
{
    TradeRecorder recorder;
    averageEntry(recorder);
    recorder.onFill(fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3), "Alpha");
    const auto trades = recorder.allTrades(marks(170.0), kD3);
    ASSERT_EQ(trades.size(), 1U);
    const auto& trade = trades.front();
    EXPECT_EQ(trade.strategy_id, kAlpha);
    EXPECT_EQ(trade.strategy_name, "Alpha");
    EXPECT_EQ(trade.direction, Direction::Long);
    EXPECT_DOUBLE_EQ(trade.entry_price, 100.0);
    EXPECT_DOUBLE_EQ(trade.peak_quantity, 4.0);
    EXPECT_EQ(trade.fill_count, 3U);
    EXPECT_DOUBLE_EQ(trade.commission, 7.8);
    EXPECT_NEAR(trade.pnl, 82.2, 1e-10);
    EXPECT_FALSE(trade.exited);
    EXPECT_TRUE(recorder.closedTrades().empty());
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), 0.0);
}

TEST(TradeRecorder, R13_FinalExitPriceAndAllFeesDefineClosedCampaign)
{
    TradeRecorder recorder;
    averageEntry(recorder);
    recorder.onFill(fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3), "Alpha");
    recorder.onFill(fill(104, OrderSide::Sell, 3.0, 140.0, 4.2, kD4), "Alpha");
    ASSERT_EQ(recorder.closedTrades().size(), 1U);
    const auto& trade = recorder.closedTrades().front();
    EXPECT_EQ(trade.start, kD1);
    EXPECT_EQ(trade.end, kD4);
    EXPECT_DOUBLE_EQ(trade.entry_price, 100.0);
    EXPECT_DOUBLE_EQ(trade.exit_price, 140.0);
    EXPECT_DOUBLE_EQ(trade.peak_quantity, 4.0);
    EXPECT_EQ(trade.fill_count, 4U);
    EXPECT_DOUBLE_EQ(trade.commission, 12.0);
    EXPECT_DOUBLE_EQ(trade.pnl, -12.0);
    EXPECT_TRUE(trade.exited);
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), -12.0);
}

TEST(TradeRecorder, R13_MarkSnapshotsDoNotRewriteCampaignEconomicsOrPriorReports)
{
    TradeRecorder recorder;
    averageEntry(recorder);
    recorder.onFill(fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3), "Alpha");
    const auto first = recorder.allTrades(marks(170.0), kD3);
    const auto down = recorder.allTrades(marks(100.0), kD4);
    const auto repeated = recorder.allTrades(marks(170.0), kD3);
    ASSERT_EQ(first.size(), 1U);
    ASSERT_EQ(down.size(), 1U);
    ASSERT_EQ(repeated.size(), 1U);
    EXPECT_NEAR(first.front().pnl, 82.2, 1e-10);
    EXPECT_NEAR(down.front().pnl, -127.8, 1e-10);
    EXPECT_NEAR(repeated.front().pnl, 82.2, 1e-10);
    EXPECT_EQ(first.front().end, kD3);
    EXPECT_EQ(down.front().end, kD4);
    EXPECT_DOUBLE_EQ(first.front().entry_price, 100.0);
    EXPECT_DOUBLE_EQ(down.front().entry_price, 100.0);
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), 0.0);
}

TEST(TradeRecorder, R14_ShortCampaignRealizesCorrectSignedProfitAndFinalExit)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Sell, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Buy, 1.0, 80.0, 0.8, kD2), "Alpha");
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), 0.0);
    recorder.onFill(fill(103, OrderSide::Buy, 1.0, 90.0, 0.9, kD3), "Alpha");
    ASSERT_EQ(recorder.closedTrades().size(), 1U);
    const auto& trade = recorder.closedTrades().front();
    EXPECT_EQ(trade.direction, Direction::Short);
    EXPECT_DOUBLE_EQ(trade.entry_price, 100.0);
    EXPECT_DOUBLE_EQ(trade.exit_price, 90.0);
    EXPECT_DOUBLE_EQ(trade.commission, 3.7);
    EXPECT_NEAR(trade.pnl, 26.3, 1e-10);
    EXPECT_EQ(trade.fill_count, 3U);
}

TEST(TradeRecorder, R14_LongToShortReversalConservesCampaignFeesAndAccountPnl)
{
    Account account(1000.0);
    TradeRecorder recorder;
    for (const auto& incoming : std::vector<Fill>{
             fill(101, OrderSide::Buy, 2.0, 100.0, 2.0),
             fill(102, OrderSide::Sell, 5.0, 120.0, 6.0, kD2)}) {
        account.applyFill(incoming);
        recorder.onFill(incoming, "Alpha");
    }
    const auto trades = recorder.allTrades(marks(110.0), kD2);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_TRUE(trades[0].exited);
    EXPECT_EQ(trades[0].direction, Direction::Long);
    EXPECT_NEAR(trades[0].pnl, 35.6, 1e-10);
    EXPECT_DOUBLE_EQ(trades[0].commission, 4.4);
    EXPECT_FALSE(trades[1].exited);
    EXPECT_EQ(trades[1].direction, Direction::Short);
    EXPECT_NEAR(trades[1].pnl, 26.4, 1e-10);
    EXPECT_DOUBLE_EQ(trades[1].commission, 3.6);
    EXPECT_EQ(trades[1].start, kD2);
    EXPECT_DOUBLE_EQ(trades[1].entry_price, 120.0);
    EXPECT_DOUBLE_EQ(account.equity(marks(110.0)), 1062.0);
    EXPECT_NEAR(trades[0].pnl + trades[1].pnl, account.equity(marks(110.0)) - 1000.0, 1e-10);
}

TEST(TradeRecorder, R14_ShortToLongReversalConservesCampaignFeesAndAccountPnl)
{
    Account account(1000.0);
    TradeRecorder recorder;
    for (const auto& incoming : std::vector<Fill>{
             fill(101, OrderSide::Sell, 2.0, 100.0, 2.0),
             fill(102, OrderSide::Buy, 5.0, 80.0, 4.0, kD2)}) {
        account.applyFill(incoming);
        recorder.onFill(incoming, "Alpha");
    }
    const auto trades = recorder.allTrades(marks(90.0), kD2);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].direction, Direction::Short);
    EXPECT_NEAR(trades[0].pnl, 36.4, 1e-10);
    EXPECT_DOUBLE_EQ(trades[0].commission, 3.6);
    EXPECT_EQ(trades[1].direction, Direction::Long);
    EXPECT_NEAR(trades[1].pnl, 27.6, 1e-10);
    EXPECT_DOUBLE_EQ(trades[1].commission, 2.4);
    EXPECT_DOUBLE_EQ(account.equity(marks(90.0)), 1064.0);
    EXPECT_NEAR(trades[0].pnl + trades[1].pnl, account.equity(marks(90.0)) - 1000.0, 1e-10);
}

TEST(TradeRecorder, R15_OpposingStrategiesRetainSeparateCampaignsAndTotalFees)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Sell, 2.0, 100.0, 2.0, kD1, kBeta), "Beta");
    const auto trades = recorder.allTrades(marks(110.0), kD2);
    ASSERT_EQ(trades.size(), 2U);
    const auto alpha = std::find_if(trades.begin(), trades.end(), [](const TradeRecord& trade) {
        return trade.strategy_id == kAlpha;
    });
    const auto beta = std::find_if(trades.begin(), trades.end(), [](const TradeRecord& trade) {
        return trade.strategy_id == kBeta;
    });
    ASSERT_NE(alpha, trades.end());
    ASSERT_NE(beta, trades.end());
    EXPECT_EQ(alpha->direction, Direction::Long);
    EXPECT_EQ(beta->direction, Direction::Short);
    EXPECT_DOUBLE_EQ(alpha->pnl, 18.0);
    EXPECT_DOUBLE_EQ(beta->pnl, -22.0);
    EXPECT_DOUBLE_EQ(alpha->pnl + beta->pnl, -4.0);
    EXPECT_TRUE(recorder.closedTrades().empty());
}

TEST(TradeRecorder, R15_ClosingBtcCannotCloseIndependentEthCampaign)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Sell, 3.0, 20.0, 0.6, kD1, kAlpha, "ETH"), "Alpha");
    recorder.onFill(fill(103, OrderSide::Sell, 2.0, 120.0, 2.4, kD2), "Alpha");
    const auto trades = recorder.allTrades(marks(120.0, 20.0), kD2);
    ASSERT_EQ(trades.size(), 2U);
    ASSERT_EQ(recorder.closedTrades().size(), 1U);
    EXPECT_EQ(recorder.closedTrades().front().coin, "BTC");
    const auto eth = std::find_if(trades.begin(), trades.end(), [](const TradeRecord& trade) {
        return trade.coin == "ETH";
    });
    ASSERT_NE(eth, trades.end());
    EXPECT_FALSE(eth->exited);
    EXPECT_EQ(eth->direction, Direction::Short);
    EXPECT_DOUBLE_EQ(eth->pnl, -0.6);
}

TEST(TradeRecorder, R15_OpeningAfterFlatStartsNewCampaignWithoutRewritingClosedTrade)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2), "Alpha");
    ASSERT_EQ(recorder.closedTrades().size(), 1U);
    const auto first = recorder.closedTrades().front();
    recorder.onFill(fill(103, OrderSide::Sell, 1.0, 130.0, 1.3, kD3), "Alpha");
    const auto trades = recorder.allTrades(marks(130.0), kD3);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].trade_id, first.trade_id);
    EXPECT_NE(trades[1].trade_id, first.trade_id);
    EXPECT_EQ(trades[0].end, kD2);
    EXPECT_NEAR(trades[0].pnl, 35.6, 1e-10);
    EXPECT_EQ(trades[1].start, kD3);
    EXPECT_EQ(trades[1].direction, Direction::Short);
    EXPECT_DOUBLE_EQ(trades[1].pnl, -1.3);
}

TEST(TradeRecorder, R15_ClosedCampaignsDoNotRequireCurrentMarks)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    recorder.onFill(fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2), "Alpha");
    const auto trades = recorder.allTrades(PriceSnapshot{}, kD4);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_TRUE(trades.front().exited);
    EXPECT_EQ(trades.front().end, kD2);
    EXPECT_DOUBLE_EQ(trades.front().exit_price, 120.0);
    EXPECT_NEAR(trades.front().pnl, 35.6, 1e-10);
}

TEST(TradeRecorder, R15_OpenCampaignsRejectMissingMarksWithoutMutatingHistory)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    EXPECT_THROW(recorder.allTrades(PriceSnapshot{}, kD2), std::runtime_error);
    const auto trades = recorder.allTrades(marks(100.0), kD2);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_DOUBLE_EQ(trades.front().pnl, -2.0);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_FALSE(trades.front().exited);
}

TEST(TradeRecorder, R15_InvalidFillDoesNotPartiallyChangeCampaignOrClosedPnl)
{
    TradeRecorder recorder;
    recorder.onFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0), "Alpha");
    auto invalid = fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2);
    invalid.price = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(recorder.onFill(invalid, "Alpha"), std::invalid_argument);
    EXPECT_TRUE(recorder.closedTrades().empty());
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), 0.0);
    const auto trades = recorder.allTrades(marks(100.0), kD2);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().fill_count, 1U);
    EXPECT_DOUBLE_EQ(trades.front().commission, 2.0);
    EXPECT_DOUBLE_EQ(trades.front().pnl, -2.0);
}

TEST(TradeRecorder, R15_ClearAndReplayRecreatesEconomicRecordsAndStableIdentity)
{
    const std::vector<Fill> fills{
        fill(101, OrderSide::Buy, 2.0, 100.0, 2.0),
        fill(102, OrderSide::Sell, 5.0, 120.0, 6.0, kD2)};
    TradeRecorder recorder;
    for (const auto& incoming : fills) recorder.onFill(incoming, "Alpha");
    const auto before = recorder.allTrades(marks(110.0), kD3);
    recorder.clear();
    EXPECT_TRUE(recorder.closedTrades().empty());
    EXPECT_DOUBLE_EQ(recorder.cumulativeClosedPnl(), 0.0);
    for (const auto& incoming : fills) recorder.onFill(incoming, "Alpha");
    const auto rebuilt = recorder.allTrades(marks(110.0), kD3);
    ASSERT_EQ(before.size(), 2U);
    ASSERT_EQ(rebuilt.size(), 2U);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(rebuilt[i].trade_id, before[i].trade_id);
        EXPECT_EQ(rebuilt[i].start, before[i].start);
        EXPECT_EQ(rebuilt[i].end, before[i].end);
        EXPECT_EQ(rebuilt[i].direction, before[i].direction);
        EXPECT_EQ(rebuilt[i].exited, before[i].exited);
        EXPECT_DOUBLE_EQ(rebuilt[i].entry_price, before[i].entry_price);
        EXPECT_DOUBLE_EQ(rebuilt[i].commission, before[i].commission);
        EXPECT_DOUBLE_EQ(rebuilt[i].pnl, before[i].pnl);
    }
    EXPECT_NEAR(recorder.cumulativeClosedPnl(), 35.6, 1e-10);
}

} // namespace
