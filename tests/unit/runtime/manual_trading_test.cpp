#include <gtest/gtest.h>

#include "manual_trading.h"
#include "runtime_examples.h"

namespace runtime_test {
namespace {

ManualControl::ManualTargetIntent manualIntent(const std::string& id, unsigned day, double btcWeight)
{
    ManualControl::ManualTargetIntent intent;
    intent.request_id = id;
    intent.correlation_id = "correlation:" + id;
    intent.actor = "synthetic operator";
    intent.request_hash = "synthetic-hash:" + id;
    intent.decision_timestamp = closeTime(day);
    intent.execution_timestamp = openTime(day + 1);
    if (btcWeight != 0.0) intent.asset_weights.emplace("BTCUSDT", btcWeight);
    intent.cash_weight = 1.0 - btcWeight;
    return intent;
}

class ManualBehaviour : public testing::Test {
protected:
    test_support::TempDir directory_{"runtime-manual"};
    MockVenue::MockExchange venue_{directory_ / "venue", chaosConfig(), matchingConfig(), accountingConfig()};
    ManualControl::ManualTrading manual_{venue_, 0.75, 0.5};
};

TEST_F(ManualBehaviour, R19_CashOnlyWhileFlatIsANoopWithoutVenueSubmission)
{
    const auto result = manual_.route(manualIntent("cash", 1, 0.0), prices(100.0), prices(125.0));
    EXPECT_EQ(result.status, ManualControl::ManualRouteStatus::Noop);
    EXPECT_FALSE(result.command_dispatched);
    EXPECT_EQ(result.submit_count, 0U);
    EXPECT_TRUE(venue_.chaos().runtime().lifecycle().orders().empty());
    EXPECT_EQ(venue_.chaos().runtime().account().marginState(openTime(2)).settled_cash, 1000.0);
}

TEST_F(ManualBehaviour, R19_AssetAndGrossLimitsRejectWithoutClippingOrDispatch)
{
    auto asset = manualIntent("asset-limit", 1, 0.6);
    auto gross = manualIntent("gross-limit", 1, 0.4);
    gross.asset_weights.emplace("ETHUSDT", 0.4);
    gross.cash_weight = 0.2;
    for (const auto& intent : {asset, gross}) {
        const auto result = manual_.route(intent, prices(100.0), prices(125.0));
        EXPECT_EQ(result.status, ManualControl::ManualRouteStatus::RiskRejected);
        EXPECT_FALSE(result.command_dispatched);
        EXPECT_FALSE(result.reason.empty());
        EXPECT_TRUE(venue_.chaos().runtime().lifecycle().orders().empty());
    }
}

TEST_F(ManualBehaviour, R19_InvalidIdentitySumMappingAndMissingCloseCannotReachVenue)
{
    for (int error = 0; error < 4; ++error) {
        auto intent = manualIntent("invalid-" + std::to_string(error), 1, 0.5);
        auto closes = prices(100.0);
        if (error == 0) intent.actor.clear();
        if (error == 1) intent.cash_weight = 0.4;
        if (error == 2) intent.asset_weights = {{"NOT_REGISTERED", 0.5}};
        if (error == 3) closes = PriceSnapshot{};
        const auto result = manual_.route(intent, closes, prices(125.0));
        EXPECT_EQ(result.status, ManualControl::ManualRouteStatus::RiskRejected);
        EXPECT_FALSE(result.command_dispatched);
        EXPECT_TRUE(venue_.chaos().runtime().lifecycle().orders().empty());
    }
}

TEST_F(ManualBehaviour, R19_CloseSizedFiveUnitsFillAndCashOnlyExitRealizes125Dollars)
{
    const auto entry = manual_.route(manualIntent("entry", 1, 0.5), prices(100.0), prices(125.0));
    ASSERT_EQ(entry.status, ManualControl::ManualRouteStatus::Submitted);
    EXPECT_TRUE(entry.command_dispatched);
    EXPECT_EQ(entry.submit_count, 1U);
    EXPECT_EQ(manual_.strategyPositions().at(ManualControl::kManualStrategyId).get("BTCUSDT"), 0.0);
    matchAtOpen(venue_, openTime(2), prices(125.0));
    EXPECT_EQ(manual_.strategyPositions().at(ManualControl::kManualStrategyId).get("BTCUSDT"), 5.0);
    const auto exit = manual_.route(manualIntent("exit", 2, 0.0), prices(125.0), prices(150.0));
    ASSERT_EQ(exit.status, ManualControl::ManualRouteStatus::Submitted);
    EXPECT_EQ(exit.submit_count, 1U);
    matchAtOpen(venue_, openTime(3), prices(150.0));
    EXPECT_EQ(manual_.strategyPositions().at(ManualControl::kManualStrategyId).get("BTCUSDT"), 0.0);
    const auto account = venue_.chaos().runtime().account().marginState(openTime(3));
    EXPECT_EQ(account.settled_cash, 1125.0);
    EXPECT_EQ(account.realized_pnl, 125.0);
    EXPECT_TRUE(venue_.reconcile(openTime(3)).clean());
}

TEST(ManualCanonicalControl, R19_DefaultGenesisStillUsesCloseSizingAndExactCashOnlyExit)
{
    test_support::TempDir directory("runtime-manual-control");
    MockVenue::MockExchange venue(directory / "venue", chaosConfig(), matchingConfig(), accountingConfig("100000.00"));
    ManualControl::ManualTrading manual(venue, 0.75, 0.5);
    const auto entry = manual.route(manualIntent("entry", 1, 0.005), prices(100.0), prices(125.0));
    ASSERT_EQ(entry.status, ManualControl::ManualRouteStatus::Submitted);
    matchAtOpen(venue, openTime(2), prices(125.0));
    EXPECT_EQ(manual.strategyPositions().at(ManualControl::kManualStrategyId).get("BTCUSDT"), 5.0);
    const auto exit = manual.route(manualIntent("exit", 2, 0.0), prices(125.0), prices(150.0));
    ASSERT_EQ(exit.status, ManualControl::ManualRouteStatus::Submitted);
    matchAtOpen(venue, openTime(3), prices(150.0));
    EXPECT_EQ(manual.strategyPositions().at(ManualControl::kManualStrategyId).get("BTCUSDT"), 0.0);
    EXPECT_EQ(venue.chaos().runtime().account().marginState(openTime(3)).settled_cash, 100125.0);
    EXPECT_EQ(venue.chaos().runtime().account().marginState(openTime(3)).realized_pnl, 125.0);
    EXPECT_TRUE(venue.reconcile(openTime(3)).clean());
}

} // namespace
} // namespace runtime_test
