#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "accounting_test_support.h"

namespace {
using namespace accounting_test;

TEST(Account, R1_BuyAndSellMoveCashAndSignedUnitsWithFees)
{
    for (const auto side : {OrderSide::Buy, OrderSide::Sell}) {
        Account account(1000.0);
        account.applyFill(fill(101, side, 2.0, 100.0, 2.0));
        EXPECT_DOUBLE_EQ(account.cash(), side == OrderSide::Buy ? 798.0 : 1198.0);
        EXPECT_DOUBLE_EQ(account.positions().get("BTC"), side == OrderSide::Buy ? 2.0 : -2.0);
        EXPECT_DOUBLE_EQ(account.equity(marks(100.0)), 998.0);
    }
}

TEST(Account, R1_ExactRoundTripLeavesOnlyNetRealizedProfit)
{
    Account account(1000.0);
    account.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    account.applyFill(fill(102, OrderSide::Sell, 2.0, 120.0, 2.4, kD2));
    EXPECT_DOUBLE_EQ(account.cash(), 1035.60);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 0.0);
    EXPECT_FALSE(account.positions().contains("BTC"));
    EXPECT_DOUBLE_EQ(account.equity(PriceSnapshot{}), 1035.60);
}

TEST(Account, R2_RepricingLongAndShortChangesOnlyMarkedEquity)
{
    Account longAccount(1000.0);
    longAccount.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    EXPECT_DOUBLE_EQ(longAccount.equity(marks(120.0)), 1038.0);
    EXPECT_DOUBLE_EQ(longAccount.equity(marks(90.0)), 978.0);
    EXPECT_DOUBLE_EQ(longAccount.cash(), 798.0);
    EXPECT_DOUBLE_EQ(longAccount.positions().get("BTC"), 2.0);
    Account shortAccount(1000.0);
    shortAccount.applyFill(fill(101, OrderSide::Sell, 2.0, 100.0, 2.0));
    EXPECT_DOUBLE_EQ(shortAccount.equity(marks(90.0)), 1018.0);
    EXPECT_DOUBLE_EQ(shortAccount.cash(), 1198.0);
    EXPECT_DOUBLE_EQ(shortAccount.positions().get("BTC"), -2.0);
}

TEST(Account, R2_MissingHeldAssetMarkIsAnErrorRatherThanZeroExposure)
{
    Account account(1000.0);
    EXPECT_DOUBLE_EQ(account.equity(PriceSnapshot{}), 1000.0);
    account.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    PriceSnapshot wrongAsset;
    wrongAsset.set("ETH", 20.0);
    EXPECT_THROW(account.equity(wrongAsset), std::runtime_error);
    EXPECT_DOUBLE_EQ(account.cash(), 798.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 2.0);
}

TEST(Account, R3_VenueCashMovementsDoNotChangePositionUnits)
{
    Account account(1000.0);
    account.applyCashMovement(3.0);
    account.applyCashMovement(-1.0);
    EXPECT_DOUBLE_EQ(account.cash(), 1002.0);
    EXPECT_DOUBLE_EQ(account.equity(PriceSnapshot{}), 1002.0);
    EXPECT_TRUE(account.positions().values().empty());
}

TEST(Account, R3_NonfiniteCashMovementLeavesExistingEconomicsAlone)
{
    Account account(1000.0);
    account.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(account.applyCashMovement(bad), std::invalid_argument);
        EXPECT_DOUBLE_EQ(account.cash(), 798.0);
        EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 2.0);
    }
}

TEST(Account, R3_RestoreAcceptsAuthoritativeZeroOrNegativeCashAndSignedPositions)
{
    Account account(1000.0);
    account.restoreState(-50.0, {{"BTC", -2.0}, {"ETH", 3.0}});
    EXPECT_DOUBLE_EQ(account.cash(), -50.0);
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), -2.0);
    EXPECT_DOUBLE_EQ(account.positions().get("ETH"), 3.0);
    EXPECT_DOUBLE_EQ(account.equity(marks(100.0)), -190.0);
    account.restoreState(0.0, {});
    EXPECT_DOUBLE_EQ(account.equity(PriceSnapshot{}), 0.0);
    EXPECT_TRUE(account.positions().values().empty());
}

TEST(Account, R3_InvalidRestoreCannotPartiallyReplaceCashOrPositions)
{
    Account account(1000.0);
    account.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(account.restoreState(bad, {}), std::invalid_argument);
        EXPECT_THROW(account.restoreState(500.0, {{"ETH", bad}}), std::invalid_argument);
        EXPECT_DOUBLE_EQ(account.cash(), 798.0);
        EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 2.0);
        EXPECT_FALSE(account.positions().contains("ETH"));
    }
}

TEST(Account, R3_InvalidFillCannotPartiallyDebitCashOrChangeUnits)
{
    for (int field = 0; field < 4; ++field) {
        for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity()}) {
            if (field == 2 && bad == 0.0) continue; // Zero commission is valid.
            Account account(1000.0);
            auto incoming = fill(101, OrderSide::Buy, 2.0, 100.0, 2.0);
            if (field == 0) incoming.quantity = bad;
            if (field == 1) incoming.price = bad;
            if (field == 2) incoming.commission = bad;
            if (field == 3) incoming.coin.clear();
            EXPECT_THROW(account.applyFill(incoming), std::invalid_argument);
            EXPECT_DOUBLE_EQ(account.cash(), 1000.0);
            EXPECT_TRUE(account.positions().values().empty());
        }
    }
}

TEST(PositionState, R3_SignedAccumulationCanFlattenAndReverseWithoutLosingOtherAssets)
{
    PositionState positions;
    positions.add("ETH", -3.0);
    positions.add("BTC", 2.0);
    positions.add("BTC", -2.0);
    EXPECT_FALSE(positions.contains("BTC"));
    EXPECT_DOUBLE_EQ(positions.get("ETH"), -3.0);
    positions.add("BTC", -2.0);
    positions.add("BTC", 5.0);
    EXPECT_DOUBLE_EQ(positions.get("BTC"), 3.0);
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW(positions.add("BTC", bad), std::invalid_argument);
        EXPECT_DOUBLE_EQ(positions.get("BTC"), 3.0);
        EXPECT_DOUBLE_EQ(positions.get("ETH"), -3.0);
    }
}

TEST(Account, R20_InitialCapitalMustBePositiveAndFinite)
{
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW({ const Account account(bad); (void)account; }, std::invalid_argument);
    }
}

TEST(Account, R2_FillPriceValuationConservesEquityAcrossIndependentLongAndShortAssets)
{
    Account account(1000.0);
    account.applyFill(fill(101, OrderSide::Buy, 2.0, 100.0, 2.0));
    account.applyFill(fill(102, OrderSide::Sell, 3.0, 20.0, 0.6, kD1, kBeta, "ETH"));
    EXPECT_DOUBLE_EQ(account.equity(marks(100.0, 20.0)), 997.40);
    account.applyFill(fill(103, OrderSide::Sell, 2.0, 120.0, 2.4, kD2));
    EXPECT_DOUBLE_EQ(account.positions().get("BTC"), 0.0);
    EXPECT_DOUBLE_EQ(account.positions().get("ETH"), -3.0);
    EXPECT_DOUBLE_EQ(account.equity(marks(120.0, 20.0)), 1035.0);
}

} // namespace
