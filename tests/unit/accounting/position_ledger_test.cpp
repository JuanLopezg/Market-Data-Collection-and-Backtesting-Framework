#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

#include "accounting_test_support.h"

namespace {
using namespace accounting_test;

std::vector<Fill> audit()
{
    return {fill(101, OrderSide::Buy, 2.0, 100.0, 2.0, kD1),
            fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2),
            fill(103, OrderSide::Sell, 1.0, 120.0, 1.2, kD2)};
}

std::vector<FundingPayment> payments()
{
    // The day before the opening fill (kD1): funding while flat is ignored.
    return {{"before-open", kD1 - 1, "BTC", 9.0},
            {"held-long", kD2, "BTC", 3.0},
            {"held-short", kD3, "BTC", -1.0}};
}

void expectAuditEconomics(const PositionLedger& ledger)
{
    ASSERT_EQ(ledger.closedPositions().size(), 1U);
    const auto& closed = ledger.closedPositions().front();
    EXPECT_TRUE(closed.long_side);
    EXPECT_DOUBLE_EQ(closed.gross_pnl, 40.0);
    EXPECT_DOUBLE_EQ(closed.fees, 4.4);
    EXPECT_DOUBLE_EQ(closed.funding, 3.0);
    EXPECT_NEAR(closed.netPnl(), 38.6, 1e-10);
    EXPECT_EQ(closed.closing_fill_id, 102U);
    ASSERT_EQ(ledger.openPositions().size(), 1U);
    const auto& open = ledger.openPositions().at("BTC");
    EXPECT_FALSE(open.long_side);
    EXPECT_DOUBLE_EQ(open.quantity, -1.0);
    EXPECT_DOUBLE_EQ(open.entry_price, 120.0);
    EXPECT_DOUBLE_EQ(open.fees, 1.2);
    EXPECT_DOUBLE_EQ(open.funding, -1.0);
    EXPECT_DOUBLE_EQ(open.netPnl(), -2.2);
    EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 40.0);
    EXPECT_DOUBLE_EQ(ledger.fees(), 5.6);
    EXPECT_DOUBLE_EQ(ledger.funding(), 2.0);
}

TEST(PositionLedger, R5_LongAndShortAddsUseQuantityWeightedAverageCost)
{
    for (const auto side : {OrderSide::Buy, OrderSide::Sell}) {
        PositionLedger ledger;
        ledger.applyFill(fill(101, side, 2.0, 100.0, 2.0));
        ledger.applyFill(fill(102, side, 2.0, 200.0, 4.0, kD2));
        ASSERT_EQ(ledger.openPositions().size(), 1U);
        const auto& lot = ledger.openPositions().at("BTC");
        EXPECT_DOUBLE_EQ(lot.quantity, side == OrderSide::Buy ? 4.0 : -4.0);
        EXPECT_EQ(lot.long_side, side == OrderSide::Buy);
        EXPECT_DOUBLE_EQ(lot.entry_price, 150.0);
        EXPECT_DOUBLE_EQ(lot.entry_value, 600.0);
        EXPECT_DOUBLE_EQ(lot.max_quantity, 4.0);
        EXPECT_DOUBLE_EQ(lot.fees, 6.0);
        EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 0.0);
    }
}

TEST(PositionLedger, R6_PartialLongReductionRealizesAtUnchangedAverageCost)
{
    PositionLedger ledger;
    averageEntry(ledger);
    EXPECT_FALSE(ledger.applyFill(fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3)).has_value());
    ASSERT_TRUE(ledger.openPositions().contains("BTC"));
    const auto& lot = ledger.openPositions().at("BTC");
    EXPECT_DOUBLE_EQ(lot.quantity, 3.0);
    EXPECT_DOUBLE_EQ(lot.entry_price, 150.0);
    EXPECT_DOUBLE_EQ(lot.entry_value, 600.0);
    EXPECT_DOUBLE_EQ(lot.gross_pnl, 30.0);
    EXPECT_DOUBLE_EQ(lot.exit_value, 180.0);
    EXPECT_DOUBLE_EQ(lot.exit_quantity, 1.0);
    EXPECT_DOUBLE_EQ(lot.fees, 7.8);
    EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 30.0);
    EXPECT_TRUE(ledger.closedPositions().empty());
}

TEST(PositionLedger, R6_PartialShortCoverReversesRealizedPnlSign)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Sell, 2.0, 100.0, 2.0));
    ledger.applyFill(fill(102, OrderSide::Sell, 2.0, 200.0, 4.0, kD2));
    ledger.applyFill(fill(103, OrderSide::Buy, 1.0, 120.0, 1.2, kD3));
    const auto& lot = ledger.openPositions().at("BTC");
    EXPECT_DOUBLE_EQ(lot.quantity, -3.0);
    EXPECT_DOUBLE_EQ(lot.entry_price, 150.0);
    EXPECT_DOUBLE_EQ(lot.gross_pnl, 30.0);
    EXPECT_DOUBLE_EQ(lot.fees, 7.2);
    EXPECT_DOUBLE_EQ(lot.exitPrice(), 120.0);
}

TEST(PositionLedger, R6_FullCloseSumsAllReductionsAndPreservesClosingIdentity)
{
    PositionLedger ledger;
    averageEntry(ledger);
    ledger.applyFill(fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3));
    const auto closed = ledger.applyFill(fill(104, OrderSide::Sell, 3.0, 140.0, 4.2, kD4));
    ASSERT_TRUE(closed.has_value());
    EXPECT_TRUE(ledger.openPositions().empty());
    ASSERT_EQ(ledger.closedPositions().size(), 1U);
    EXPECT_DOUBLE_EQ(closed->quantity, 0.0);
    EXPECT_DOUBLE_EQ(closed->gross_pnl, 0.0);
    EXPECT_DOUBLE_EQ(closed->fees, 12.0);
    EXPECT_DOUBLE_EQ(closed->netPnl(), -12.0);
    EXPECT_DOUBLE_EQ(closed->exit_value, 600.0);
    EXPECT_DOUBLE_EQ(closed->exit_quantity, 4.0);
    EXPECT_DOUBLE_EQ(closed->exitPrice(), 150.0);
    EXPECT_EQ(closed->opened_at, kD1);
    EXPECT_EQ(closed->closed_at, kD4);
    EXPECT_EQ(closed->closing_fill_id, 104U);
}

TEST(PositionLedger, R7_AddAfterReductionWeightsOnlyRemainingCostBasis)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0));
    ledger.applyFill(fill(102, OrderSide::Buy, 2.0, 200.0, 0.0, kD2));
    ledger.applyFill(fill(103, OrderSide::Sell, 1.0, 180.0, 0.0, kD3));
    ledger.applyFill(fill(104, OrderSide::Buy, 1.0, 210.0, 0.0, kD4));
    const auto& lot = ledger.openPositions().at("BTC");
    EXPECT_DOUBLE_EQ(lot.quantity, 4.0);
    EXPECT_DOUBLE_EQ(lot.entry_price, 165.0);
    EXPECT_DOUBLE_EQ(lot.entry_value, 810.0);
    EXPECT_DOUBLE_EQ(lot.gross_pnl, 30.0);
    EXPECT_DOUBLE_EQ(lot.max_quantity, 4.0);
}

TEST(PositionLedger, R8_LongToShortClosesOldLotAndSplitsReversalFee)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    const auto closed = ledger.applyFill(fill(102, OrderSide::Sell, 5.0, 120.0, 6.0, kD2));
    ASSERT_TRUE(closed.has_value());
    EXPECT_DOUBLE_EQ(closed->gross_pnl, 40.0);
    EXPECT_DOUBLE_EQ(closed->fees, 4.4);
    EXPECT_NEAR(closed->netPnl(), 35.6, 1e-10);
    const auto& remainder = ledger.openPositions().at("BTC");
    EXPECT_FALSE(remainder.long_side);
    EXPECT_DOUBLE_EQ(remainder.quantity, -3.0);
    EXPECT_DOUBLE_EQ(remainder.entry_price, 120.0);
    EXPECT_DOUBLE_EQ(remainder.fees, 3.6);
    EXPECT_EQ(remainder.opened_at, kD2);
    EXPECT_DOUBLE_EQ(ledger.fees(), 8.0);
}

TEST(PositionLedger, R8_ShortToLongClosesOldLotAndKeepsPositiveRemainder)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Sell, 2.0, 100.0, 2.0));
    const auto closed = ledger.applyFill(fill(102, OrderSide::Buy, 5.0, 80.0, 4.0, kD2));
    ASSERT_TRUE(closed.has_value());
    EXPECT_FALSE(closed->long_side);
    EXPECT_DOUBLE_EQ(closed->gross_pnl, 40.0);
    EXPECT_DOUBLE_EQ(closed->fees, 3.6);
    EXPECT_NEAR(closed->netPnl(), 36.4, 1e-10);
    const auto& remainder = ledger.openPositions().at("BTC");
    EXPECT_TRUE(remainder.long_side);
    EXPECT_DOUBLE_EQ(remainder.quantity, 3.0);
    EXPECT_DOUBLE_EQ(remainder.entry_price, 80.0);
    EXPECT_DOUBLE_EQ(remainder.fees, 2.4);
    EXPECT_EQ(remainder.opened_at, kD2);
    EXPECT_DOUBLE_EQ(ledger.fees(), 6.0);
}

TEST(PositionLedger, R9_AccountLotsNetStrategiesButKeepAssetsSeparate)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    ledger.applyFill(fill(102, OrderSide::Sell, 3.0, 20.0, 0.6, kD1, kBeta, "ETH"));
    ledger.applyFill(fill(103, OrderSide::Sell, 2.0, 120.0, 2.4, kD2, kBeta));
    ASSERT_EQ(ledger.closedPositions().size(), 1U);
    EXPECT_EQ(ledger.closedPositions().front().coin, "BTC");
    EXPECT_NEAR(ledger.closedPositions().front().netPnl(), 35.6, 1e-10);
    ASSERT_EQ(ledger.openPositions().size(), 1U);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("ETH").quantity, -3.0);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("ETH").fees, 0.6);
    EXPECT_DOUBLE_EQ(ledger.fees(), 5.0);
}

TEST(PositionLedger, R10_DuplicateOpeningFillCannotIncreaseUnitsOrFees)
{
    PositionLedger ledger;
    const auto incoming = fill(101, OrderSide::Buy, 2.0, 100.0, 2.0);
    ledger.applyFill(incoming);
    ledger.applyFill(incoming);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").quantity, 2.0);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").entry_value, 200.0);
    EXPECT_DOUBLE_EQ(ledger.fees(), 2.0);
}

TEST(PositionLedger, R10_DuplicateClosingFillCannotOpenOppositeLotOrRepeatProfit)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    const auto closing = fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2);
    ASSERT_TRUE(ledger.applyFill(closing).has_value());
    EXPECT_FALSE(ledger.applyFill(closing).has_value());
    EXPECT_EQ(ledger.closedPositions().size(), 1U);
    EXPECT_TRUE(ledger.openPositions().empty());
    EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 40.0);
    EXPECT_DOUBLE_EQ(ledger.fees(), 4.4);
}

TEST(PositionLedger, R10_InvalidFillCannotConsumeCorrectedEconomicIdentity)
{
    for (int invalid = 0; invalid < 3; ++invalid) {
        PositionLedger ledger;
        ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
        const auto corrected = fill(102, OrderSide::Buy, 2.0, 200.0, 4.0, kD2);
        auto bad = corrected;
        if (invalid == 0) bad.quantity = -1.0;
        if (invalid == 1) bad.price = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 2) bad.commission = -1.0;
        EXPECT_THROW(ledger.applyFill(bad), std::invalid_argument);
        ledger.applyFill(corrected);
        EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").quantity, 4.0);
        EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").entry_price, 150.0);
        EXPECT_DOUBLE_EQ(ledger.fees(), 6.0);
    }
}

TEST(PositionLedger, R11_FundingIsDeduplicatedAndIncludedInClosedNetPnl)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    const FundingPayment receipt{"receipt", kD2, "BTC", 3.0};
    ledger.applyFunding(receipt);
    ledger.applyFunding(receipt);
    const auto closed = ledger.applyFill(fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD3));
    ASSERT_TRUE(closed.has_value());
    EXPECT_DOUBLE_EQ(closed->funding, 3.0);
    EXPECT_NEAR(closed->netPnl(), 38.6, 1e-10);
    EXPECT_DOUBLE_EQ(ledger.funding(), 3.0);
}

TEST(PositionLedger, R11_FundingWhileFlatCannotAttachToALaterPositionOnRedelivery)
{
    PositionLedger ledger;
    const FundingPayment earlier{"before-open", kD1, "BTC", 9.0};
    ledger.applyFunding(earlier);
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0, kD2));
    ledger.applyFunding(earlier);
    EXPECT_DOUBLE_EQ(ledger.funding(), 0.0);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").funding, 0.0);
}

TEST(PositionLedger, R11_FundingIsSignedAndAssetSpecific)
{
    PositionLedger ledger;
    ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    ledger.applyFill(fill(102, OrderSide::Sell, 3.0, 20.0, 0.6, kD1, kBeta, "ETH"));
    ledger.applyFunding({"btc-credit", kD2, "BTC", 3.0});
    ledger.applyFunding({"btc-debit", kD3, "BTC", -1.0});
    ledger.applyFunding({"eth-credit", kD2, "ETH", 5.0});
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").funding, 2.0);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("ETH").funding, 5.0);
    EXPECT_DOUBLE_EQ(ledger.funding(), 7.0);
}

TEST(PositionLedger, R11_InvalidFundingDoesNotBlockCorrectedPaymentId)
{
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        PositionLedger ledger;
        ledger.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0));
        EXPECT_THROW(ledger.applyFunding({"receipt", kD2, "BTC", bad}), std::invalid_argument);
        ledger.applyFunding({"receipt", kD2, "BTC", 3.0});
        EXPECT_DOUBLE_EQ(ledger.funding(), 3.0);
    }
}

TEST(PositionLedger, R12_RebuildSortsAuditAndAppliesFundingBeforeSameTimeFills)
{
    auto fills = audit();
    std::reverse(fills.begin(), fills.end());
    auto funding = payments();
    std::reverse(funding.begin(), funding.end());
    expectAuditEconomics(PositionLedger::rebuild(fills, funding));
}

// Live timestamps are whole days: funding dated on the day a position opens (fills at the
// 00:00 open, funding hourly after) belongs to that new position (2026-10-23, XRP).
TEST(PositionLedger, R12_SameDayFundingWithoutAPositionJoinsTheOneOpenedThatDay)
{
    const auto ledger = PositionLedger::rebuild(
        {fill(101, OrderSide::Buy, 17.0, 1.3651, 0.0159, kD2)},
        {{"xrp-1", kD2, "BTC", -0.003}, {"xrp-2", kD2, "BTC", -0.0017}});
    EXPECT_NEAR(ledger.openPositions().at("BTC").funding, -0.0047, 1e-12);
    EXPECT_NEAR(ledger.funding(), -0.0047, 1e-12);
}

TEST(PositionLedger, R12_SameTimestampClosingAndOpeningFillsFollowFillId)
{
    const auto fills = audit();
    // Closing 102 must precede opening short 103 even in reverse input order.
    const auto ledger = PositionLedger::rebuild({fills[2], fills[1], fills[0]}, {});
    ASSERT_EQ(ledger.closedPositions().size(), 1U);
    EXPECT_EQ(ledger.closedPositions().front().closing_fill_id, 102U);
    EXPECT_DOUBLE_EQ(ledger.closedPositions().front().gross_pnl, 40.0);
    EXPECT_DOUBLE_EQ(ledger.closedPositions().front().fees, 4.4);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").quantity, -1.0);
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").fees, 1.2);
}

TEST(PositionLedger, R12_DuplicateAuditRowsAndRepeatedRebuildPreserveEconomics)
{
    auto fills = audit();
    fills.push_back(fills.front());
    fills.push_back(fills[1]);
    auto funding = payments();
    funding.push_back(funding[1]);
    const auto first = PositionLedger::rebuild(fills, funding);
    const auto restarted = PositionLedger::rebuild(fills, funding);
    expectAuditEconomics(first);
    expectAuditEconomics(restarted);
}

TEST(PositionLedger, R12_RebuiltLedgerRetainsDeduplicationAndAcceptsNewFills)
{
    auto fills = audit();
    auto ledger = PositionLedger::rebuild(fills, payments());
    ledger.applyFill(fills.back());
    EXPECT_DOUBLE_EQ(ledger.openPositions().at("BTC").quantity, -1.0);
    const auto closed = ledger.applyFill(fill(104, OrderSide::Buy, 1.0, 100.0, 1.0, kD4));
    ASSERT_TRUE(closed.has_value());
    EXPECT_FALSE(closed->long_side);
    EXPECT_DOUBLE_EQ(closed->gross_pnl, 20.0);
    EXPECT_DOUBLE_EQ(closed->fees, 2.2);
    EXPECT_DOUBLE_EQ(closed->funding, -1.0);
    EXPECT_NEAR(closed->netPnl(), 16.8, 1e-10);
    EXPECT_TRUE(ledger.openPositions().empty());
    EXPECT_EQ(ledger.closedPositions().size(), 2U);
    EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 60.0);
}

TEST(PositionLedger, R6_RealizedUnrealizedFeesAndFundingReconcileToAccountEquity)
{
    Account account(1000.0);
    PositionLedger ledger;
    const std::vector<Fill> fills{
        fill(101, OrderSide::Buy, 2.0, 100.0, 2.0),
        fill(102, OrderSide::Buy, 2.0, 200.0, 4.0, kD2),
        fill(103, OrderSide::Sell, 1.0, 180.0, 1.8, kD3)};
    for (const auto& incoming : fills) {
        account.applyFill(incoming);
        ledger.applyFill(incoming);
    }
    account.applyCashMovement(3.0);
    ledger.applyFunding({"receipt", kD3, "BTC", 3.0});
    EXPECT_DOUBLE_EQ(account.cash(), 575.2);
    EXPECT_DOUBLE_EQ(account.equity(marks(170.0)), 1085.2);
    EXPECT_DOUBLE_EQ(ledger.realizedGrossPnl(), 30.0);
    const auto& lot = ledger.openPositions().at("BTC");
    const double unrealized = (170.0 - lot.entry_price) * lot.quantity;
    EXPECT_DOUBLE_EQ(unrealized, 60.0);
    EXPECT_NEAR(account.equity(marks(170.0)) - 1000.0,
        ledger.realizedGrossPnl() + unrealized - ledger.fees() + ledger.funding(), 1e-10);
}

} // namespace
