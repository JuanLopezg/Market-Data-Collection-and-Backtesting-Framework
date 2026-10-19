#include <algorithm>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "reconciler.h"
#include "snapshot_examples.h"

namespace persistence_test {
namespace {

class ReconcilerBehaviour : public testing::Test {
protected:
    TradingStateSnapshot local_ = exampleSnapshot();
    ExchangeSnapshot exchange_;

    void SetUp() override
    {
        exchange_.timestamp = 20260102;
        exchange_.cash = 849.0;
        exchange_.positions = {{"BTC", 2.0}, {"ETH", -0.5}};
        exchange_.open_orders.push_back({41, "venue-41", "BTC", OrderSide::Buy, 5.0, 2.0});
    }

    ReconciliationReport compare(const Reconciler& reconciler = Reconciler()) const
    {
        return reconciler.compare(local_, exchange_);
    }

    static void expectIssue(const ReconciliationReport& report, ReconciliationIssueKind kind,
                            const Coin& coin, OrderID order, double localValue, double venueValue)
    {
        ASSERT_EQ(report.issues.size(), 1U);
        const auto& issue = report.issues.front();
        EXPECT_EQ(issue.kind, kind);
        EXPECT_EQ(issue.coin, coin);
        EXPECT_EQ(issue.order_id, order);
        EXPECT_EQ(issue.local_value, localValue);
        EXPECT_EQ(issue.exchange_value, venueValue);
        EXPECT_FALSE(issue.message.empty());
    }
};

TEST_F(ReconcilerBehaviour, R9_MatchingStateIsReadOnlyAndRepeatable)
{
    const auto before = local_;
    EXPECT_TRUE(compare().clean());
    EXPECT_TRUE(compare().clean());
    expectSnapshot(local_, before);
    EXPECT_EQ(exchange_.cash, 849.0);
    EXPECT_EQ(exchange_.positions.at("BTC"), 2.0);
    ASSERT_EQ(exchange_.open_orders.size(), 1U);
    EXPECT_EQ(exchange_.open_orders.front().filled_quantity, 2.0);
    // Pending targets/strategy signals in exampleSnapshot are intentionally not
    // exchange holdings; comparing them would report false economic drift.
}

TEST_F(ReconcilerBehaviour, R9_CashToleranceIsInclusiveAndReportsRealDrift)
{
    local_.account_cash = 1000.0;
    exchange_.cash = 1000.125;
    EXPECT_TRUE(compare(Reconciler(0.125)).clean());
    exchange_.cash = 1000.25;
    expectIssue(compare(Reconciler(0.125)), ReconciliationIssueKind::CashMismatch,
                "", 0, 1000.0, 1000.25);
    local_.account_cash = 0.0;
    exchange_.cash = 1e-6;
    EXPECT_TRUE(compare().clean());
    exchange_.cash = 2e-6;
    expectIssue(compare(), ReconciliationIssueKind::CashMismatch, "", 0, 0.0, 2e-6);
}

TEST_F(ReconcilerBehaviour, R9_CollateralIsNotCash)
{
    exchange_.collateral_usd = 500.0;
    EXPECT_TRUE(compare().clean());
}

TEST_F(ReconcilerBehaviour, R10_PositionUnionFindsLocalOnlyAndExchangeOnlyAssets)
{
    local_.account_positions = {{"BTC", 2.0}};
    exchange_.positions = {{"ETH", -0.5}};
    auto report = compare();
    ASSERT_EQ(report.issues.size(), 2U);
    std::sort(report.issues.begin(), report.issues.end(),
        [](const auto& a, const auto& b) { return a.coin < b.coin; });
    EXPECT_EQ(report.issues[0].kind, ReconciliationIssueKind::PositionMismatch);
    EXPECT_EQ(report.issues[0].coin, "BTC");
    EXPECT_EQ(report.issues[0].local_value, 2.0);
    EXPECT_EQ(report.issues[0].exchange_value, 0.0);
    EXPECT_EQ(report.issues[1].kind, ReconciliationIssueKind::PositionMismatch);
    EXPECT_EQ(report.issues[1].coin, "ETH");
    EXPECT_EQ(report.issues[1].local_value, 0.0);
    EXPECT_EQ(report.issues[1].exchange_value, -0.5);
}

TEST_F(ReconcilerBehaviour, R10_PositionSignMattersAndAbsentEqualsZero)
{
    local_.account_positions["SOL"] = 0.0;
    exchange_.positions["XRP"] = 0.0;
    EXPECT_TRUE(compare().clean());
    exchange_.positions["BTC"] = -2.0;
    expectIssue(compare(), ReconciliationIssueKind::PositionMismatch, "BTC", 0, 2.0, -2.0);
}

TEST_F(ReconcilerBehaviour, R11_AbsoluteQuantityToleranceIncludesItsBoundary)
{
    exchange_.positions["BTC"] = 2.125;
    EXPECT_TRUE(compare(Reconciler(1e-6, 0.125)).clean());
    exchange_.positions["BTC"] = 2.25;
    expectIssue(compare(Reconciler(1e-6, 0.125)),
                ReconciliationIssueKind::PositionMismatch, "BTC", 0, 2.0, 2.25);
}

TEST_F(ReconcilerBehaviour, R11_LargeHoldingsAllowRoundoffButRejectSizeStepDrift)
{
    for (const double sign : {-1.0, 1.0}) {
        SCOPED_TRACE(sign);
        local_.account_positions["BTC"] = sign * 1000000.0;
        exchange_.positions["BTC"] = sign * 1000000.0000005;
        EXPECT_TRUE(compare().clean());
        exchange_.positions["BTC"] = sign * 1000000.001;
        expectIssue(compare(), ReconciliationIssueKind::PositionMismatch, "BTC", 0,
                    sign * 1000000.0, sign * 1000000.001);
    }
}

TEST_F(ReconcilerBehaviour, R12_LocalOrderIdMatchesBeforeExternalId)
{
    exchange_.open_orders.front().exchange_order_id = "venue-alternate-display";
    EXPECT_TRUE(compare().clean());
    // The exchange-ID match has a wrong size; the correct local-ID match wins.
    exchange_.open_orders.insert(exchange_.open_orders.begin(),
        {0, "venue-41", "BTC", OrderSide::Buy, 6.0, 2.0});
    expectIssue(compare(), ReconciliationIssueKind::UnexpectedExchangeOrder, "BTC", 0, 0.0, 4.0);
}

TEST_F(ReconcilerBehaviour, R12_ExternalIdMatchesWhenLocalMappingIsUnavailable)
{
    exchange_.open_orders.front().local_order_id = 0;
    EXPECT_TRUE(compare().clean());
    local_.orders.front().exchange_order_id.clear();
    const auto report = compare();
    ASSERT_EQ(report.issues.size(), 2U);
    EXPECT_EQ(std::count_if(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.kind == ReconciliationIssueKind::MissingExchangeOrder && issue.order_id == 41;
    }), 1);
    EXPECT_EQ(std::count_if(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.kind == ReconciliationIssueKind::UnexpectedExchangeOrder && issue.order_id == 0;
    }), 1);
}

TEST_F(ReconcilerBehaviour, R12_OrderVectorPermutationDoesNotChangeMatching)
{
    auto second = exampleOrder(42);
    second.request.coin = "ETH";
    second.request.side = OrderSide::Sell;
    second.request.quantity = 4.0;
    second.filled_quantity = 1.0;
    second.exchange_order_id = "venue-42";
    local_.orders.push_back(second);
    exchange_.open_orders.push_back({42, "venue-42", "ETH", OrderSide::Sell, 4.0, 1.0});
    EXPECT_TRUE(compare().clean());
    std::reverse(exchange_.open_orders.begin(), exchange_.open_orders.end());
    EXPECT_TRUE(compare().clean());
    std::reverse(local_.orders.begin(), local_.orders.end());
    EXPECT_TRUE(compare().clean());
}

TEST_F(ReconcilerBehaviour, R12_OrderSymbolSideTotalAndFilledQuantityAreCompared)
{
    const auto matching = exchange_.open_orders.front();
    for (int field = 0; field < 4; ++field) {
        SCOPED_TRACE(field);
        auto& order = exchange_.open_orders.front();
        order = matching;
        if (field == 0) order.coin = "ETH";
        if (field == 1) order.side = OrderSide::Sell;
        if (field == 2) order.quantity = 6.0;
        if (field == 3) order.filled_quantity = 3.0;
        expectIssue(compare(), ReconciliationIssueKind::OrderMismatch, "BTC", 41,
                    3.0, field == 2 ? 4.0 : (field == 3 ? 2.0 : 3.0));
    }
    exchange_.open_orders.front().quantity = 6.0;
    exchange_.open_orders.front().filled_quantity = 3.0;
    expectIssue(compare(), ReconciliationIssueKind::OrderMismatch, "BTC", 41, 3.0, 3.0);
}

TEST_F(ReconcilerBehaviour, R11_OrderTotalAndPartialFillsUseQuantityTolerances)
{
    auto& localOrder = local_.orders.front();
    auto& venueOrder = exchange_.open_orders.front();
    venueOrder.quantity = 5.125;
    venueOrder.filled_quantity = 2.125;
    EXPECT_TRUE(compare(Reconciler(1e-6, 0.125)).clean());
    venueOrder.filled_quantity = 2.25;
    expectIssue(compare(Reconciler(1e-6, 0.125)),
                ReconciliationIssueKind::OrderMismatch, "BTC", 41, 3.0, 2.875);
    localOrder.request.quantity = 2000000.0;
    localOrder.filled_quantity = 1000000.0;
    venueOrder.quantity = 2000000.000001;
    venueOrder.filled_quantity = 1000000.0000005;
    EXPECT_TRUE(compare().clean());
    venueOrder.quantity = 2000000.001;
    const auto report = compare();
    ASSERT_EQ(report.issues.size(), 1U);
    EXPECT_EQ(report.issues.front().kind, ReconciliationIssueKind::OrderMismatch);
}

TEST_F(ReconcilerBehaviour, R13_OnlyUnsentCreatedOrderMayBeAbsentAtExchange)
{
    exchange_.open_orders.clear();
    local_.orders.front().filled_quantity = 0.0;
    local_.orders.front().status = ExecutionOrderStatus::Created;
    EXPECT_TRUE(compare().clean());
    for (const auto status : {ExecutionOrderStatus::Submitted, ExecutionOrderStatus::Accepted,
                              ExecutionOrderStatus::PartiallyFilled}) {
        SCOPED_TRACE(static_cast<int>(status));
        local_.orders.front().status = status;
        expectIssue(compare(), ReconciliationIssueKind::MissingExchangeOrder, "BTC", 41, 5.0, 0.0);
    }
}

TEST_F(ReconcilerBehaviour, R13_TerminalOrdersNeedNoVenueOrderButStaleVenueOrdersBlock)
{
    const auto venueOrder = exchange_.open_orders.front();
    for (const auto status : {ExecutionOrderStatus::Filled, ExecutionOrderStatus::Canceled,
                              ExecutionOrderStatus::Rejected}) {
        SCOPED_TRACE(static_cast<int>(status));
        local_.orders.front().status = status;
        exchange_.open_orders.clear();
        EXPECT_TRUE(compare().clean());
        exchange_.open_orders.push_back(venueOrder);
        expectIssue(compare(), ReconciliationIssueKind::UnexpectedExchangeOrder, "BTC", 41, 0.0, 3.0);
    }
}

TEST_F(ReconcilerBehaviour, R13_UnexpectedExchangeOrderReportsRemainingExposure)
{
    exchange_.open_orders.push_back({99, "foreign", "ETH", OrderSide::Sell, 4.0, 1.0});
    expectIssue(compare(), ReconciliationIssueKind::UnexpectedExchangeOrder, "ETH", 99, 0.0, 3.0);
}

TEST_F(ReconcilerBehaviour, R12_OneVenueOrderCannotSatisfyTwoLocalClaims)
{
    auto duplicateClaim = local_.orders.front();
    duplicateClaim.request.order_id = 42;
    local_.orders.push_back(duplicateClaim);
    expectIssue(compare(), ReconciliationIssueKind::MissingExchangeOrder, "BTC", 42, 3.0, 0.0);
    // Conversely, a duplicated venue row must not be swallowed by one local match.
    local_.orders.pop_back();
    exchange_.open_orders.push_back(exchange_.open_orders.front());
    expectIssue(compare(), ReconciliationIssueKind::UnexpectedExchangeOrder, "BTC", 41, 0.0, 3.0);
}

TEST_F(ReconcilerBehaviour, R13_CancelRequestIsNotCancelConfirmation)
{
    ASSERT_TRUE(local_.orders.front().cancel_requested);
    EXPECT_TRUE(compare().clean());
    exchange_.open_orders.clear();
    expectIssue(compare(), ReconciliationIssueKind::MissingExchangeOrder, "BTC", 41, 3.0, 0.0);
}

TEST_F(ReconcilerBehaviour, R14_InvalidTolerancesAreRejected)
{
    for (const double bad : {-1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW((void)Reconciler(bad, 1e-10), std::invalid_argument);
        EXPECT_THROW((void)Reconciler(1e-6, bad), std::invalid_argument);
    }
}

TEST_F(ReconcilerBehaviour, R14_NonfiniteCashPositionAndOrderValuesAlwaysBlock)
{
    const auto originalLocal = local_;
    const auto originalExchange = exchange_;
    for (const double bad : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        for (int field = 0; field < 4; ++field) {
            for (const bool badLocal : {false, true}) {
                SCOPED_TRACE(field);
                SCOPED_TRACE(badLocal);
                local_ = originalLocal;
                exchange_ = originalExchange;
                if (field == 0) {
                    if (badLocal) local_.account_cash = bad;
                    else exchange_.cash = bad;
                } else if (field == 1) {
                    if (badLocal) local_.account_positions["BTC"] = bad;
                    else exchange_.positions["BTC"] = bad;
                } else if (field == 2) {
                    if (badLocal) local_.orders.front().request.quantity = bad;
                    else exchange_.open_orders.front().quantity = bad;
                } else {
                    if (badLocal) local_.orders.front().filled_quantity = bad;
                    else exchange_.open_orders.front().filled_quantity = bad;
                }
                const auto report = compare();
                ASSERT_EQ(report.issues.size(), 1U);
                EXPECT_EQ(report.issues.front().kind,
                    field == 0 ? ReconciliationIssueKind::CashMismatch :
                    field == 1 ? ReconciliationIssueKind::PositionMismatch : ReconciliationIssueKind::OrderMismatch);
            }
        }
    }
    local_ = originalLocal;
    exchange_ = originalExchange;
    local_.account_positions["BTC"] = std::numeric_limits<double>::infinity();
    exchange_.positions["BTC"] = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(compare().clean());
}

TEST_F(ReconcilerBehaviour, R14_ZeroToleranceAllowsEqualityButNotMaterialDifference)
{
    EXPECT_TRUE(compare(Reconciler(0.0, 0.0)).clean());
    exchange_.positions["BTC"] = 2.0000001;
    expectIssue(compare(Reconciler(0.0, 0.0)),
                ReconciliationIssueKind::PositionMismatch, "BTC", 0, 2.0, 2.0000001);
}

} // namespace
} // namespace persistence_test
