#include <gtest/gtest.h>

#include "risk_test_data.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace risk_test;

TEST(PortfolioRisk, R12_EquityIncludesCollateralAndSignedRealPositionsBeforeAllocation)
{
    auto snapshot = account();
    snapshot.collateral_usd = 200;
    snapshot.positions = {{"A", 2}, {"B", -1}};
    auto engine = makeEngine({.allocation = 0.4});
    const auto batch = engine.onSignals(signals({{"A", 1}, {"B", -0.5}}), marks(), snapshot);
    const auto& approved = decision(batch);
    EXPECT_DOUBLE_EQ(approved.reference_capital, 540);
    EXPECT_DOUBLE_EQ(approved.target_notional_usd.at("A"), 135);
    EXPECT_DOUBLE_EQ(approved.target_notional_usd.at("B"), -67.5);
    EXPECT_DOUBLE_EQ(snapshot.cash, 1000);
    EXPECT_DOUBLE_EQ(snapshot.collateral_usd, 200);
    EXPECT_DOUBLE_EQ(snapshot.positions.at("A"), 2);
    EXPECT_DOUBLE_EQ(snapshot.positions.at("B"), -1);
}

TEST(PortfolioRisk, R12_IndependentStrategiesReceiveTheirOwnCapitalAndSignedTargets)
{
    std::vector<PortfolioRiskStrategyConfig> configs;
    configs.push_back(makeConfig({.allocation = 0.6}));
    configs.push_back(makeConfig({.id = 9, .allocation = 0.4, .full_weight = 0.5}));
    PortfolioRiskEngine engine(std::move(configs));
    auto input = signals();
    input.strategies.push_back({9, "Pure_RSI", {{"A", -1}}});
    auto snapshot = account();
    snapshot.strategy_positions.emplace(9, Opinions{});
    const auto batch = engine.onSignals(input, marks(), snapshot);
    EXPECT_DOUBLE_EQ(decision(batch, 7).reference_capital, 600);
    EXPECT_DOUBLE_EQ(decision(batch, 9).reference_capital, 400);
    EXPECT_DOUBLE_EQ(decision(batch, 7).target_notional_usd.at("A"), 150);
    EXPECT_DOUBLE_EQ(decision(batch, 9).target_notional_usd.at("A"), -200);
}

TEST(PortfolioRisk, R13_AssetAndGrossCapsAreAppliedBeforeWeightsBecomeUsdTargets)
{
    auto engine = makeEngine({.allocation = 0.5, .full_weight = 0.8,
                              .gross_cap = 0.6, .asset_cap = 0.5});
    const auto batch = engine.onSignals(signals({{"A", 1}, {"B", -0.75}, {"C", 0.25}}),
                                         marks(), account());
    const auto& approved = decision(batch);
    EXPECT_DOUBLE_EQ(approved.reference_capital, 500);
    EXPECT_NEAR(approved.target_notional_usd.at("A"), 125, 1e-10);
    EXPECT_NEAR(approved.target_notional_usd.at("B"), -125, 1e-10);
    EXPECT_NEAR(approved.target_notional_usd.at("C"), 50, 1e-10);
    EXPECT_DOUBLE_EQ(approved.decisions.at("A").target_weight, 0.25);
    EXPECT_DOUBLE_EQ(approved.decisions.at("B").target_weight, -0.25);
    EXPECT_EQ(approved.decision_timestamp, date);
}

TEST(PortfolioRisk, R13_HoldKeepsFilledQuantityEvenWhenTheCappedEvaluationDiffers)
{
    auto snapshot = account(800);
    snapshot.positions = {{"A", 2}};
    snapshot.strategy_positions.at(7) = {{"A", 2}};
    auto engine = makeEngine({.full_weight = 0.8, .asset_cap = 0.1});
    std::vector<PortfolioRiskEvaluation> observed;
    const auto batch = engine.onSignals(signals(), marks(), snapshot, &observed);
    EXPECT_TRUE(batch.strategies.empty());
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_DOUBLE_EQ(observed[0].sized_weights.get("A"), 0.8);
    EXPECT_DOUBLE_EQ(observed[0].approved_weights.get("A"), 0.1);
    EXPECT_EQ(observed[0].rebalance_decisions.at("A").action, RebalanceAction::Hold);
    EXPECT_DOUBLE_EQ(observed[0].current_quantities.at("A"), 2);
    EXPECT_DOUBLE_EQ(snapshot.strategy_positions.at(7).at("A"), 2);
    EXPECT_DOUBLE_EQ(snapshot.cash, 800);
}

TEST(PortfolioRisk, R13_DisappearedSignalsFlattenActualLongAndShortHoldingsAtZeroNotional)
{
    auto snapshot = account();
    snapshot.positions = {{"A", 2}, {"B", -3}};
    snapshot.strategy_positions.at(7) = snapshot.positions;
    auto engine = makeEngine();
    const auto batch = engine.onSignals(signals({}), marks(), snapshot);
    const auto& approved = decision(batch);
    for (const Coin coin : {"A", "B"}) {
        EXPECT_EQ(approved.decisions.at(coin).action, RebalanceAction::Flat);
        EXPECT_DOUBLE_EQ(approved.target_notional_usd.at(coin), 0);
    }
    EXPECT_DOUBLE_EQ(snapshot.positions.at("A"), 2);
    EXPECT_DOUBLE_EQ(snapshot.positions.at("B"), -3);
}

TEST(PortfolioRisk, R13_ZeroRiskLimitFlattensAnActiveHeldSignal)
{
    auto snapshot = account(800);
    snapshot.positions = {{"A", 2}};
    snapshot.strategy_positions.at(7) = snapshot.positions;
    auto engine = makeEngine({.asset_cap = 0});
    const auto batch = engine.onSignals(signals(), marks(), snapshot);
    EXPECT_EQ(decision(batch).decisions.at("A").action, RebalanceAction::Flat);
    EXPECT_DOUBLE_EQ(decision(batch).target_notional_usd.at("A"), 0);
    EXPECT_DOUBLE_EQ(snapshot.strategy_positions.at(7).at("A"), 2);
}

TEST(PortfolioRisk, R6_UnavailableSizingProducesNoIntentAndDoesNotLiquidateHoldings)
{
    auto data = marks();
    data.erase(20240101);
    data.erase(20240102);
    auto snapshot = account(800);
    snapshot.positions = {{"A", 2}};
    snapshot.strategy_positions.at(7) = snapshot.positions;
    auto engine = makeEngine({}, volSizer());
    std::vector<PortfolioRiskEvaluation> observed;
    const auto batch = engine.onSignals(signals(), data, snapshot, &observed);
    EXPECT_TRUE(batch.strategies.empty());
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_FALSE(observed[0].sizing_available);
    EXPECT_DOUBLE_EQ(snapshot.strategy_positions.at(7).at("A"), 2);
}

TEST(PortfolioRisk, R6_AvailableEmptyVolatilitySizingStillClosesHoldingsWithoutHistory)
{
    auto data = marks();
    data.erase(20240101);
    data.erase(20240102);
    auto snapshot = account(800);
    snapshot.positions = {{"A", 2}};
    snapshot.strategy_positions.at(7) = snapshot.positions;
    auto engine = makeEngine({}, volSizer());
    const auto batch = engine.onSignals(signals({}), data, snapshot);
    EXPECT_EQ(decision(batch).decisions.at("A").action, RebalanceAction::Flat);
    EXPECT_DOUBLE_EQ(decision(batch).target_notional_usd.at("A"), 0);
}

TEST(PortfolioRisk, R18_VolatilityDiagnosticsReportTheSameScaleAndCapsAsTheUsdIntent)
{
    auto engine = makeEngine({.asset_cap = 0.25}, volSizer());
    std::vector<PortfolioRiskEvaluation> observed;
    const auto batch = engine.onSignals(signals(), volatilityHistory(), account(), &observed);
    EXPECT_DOUBLE_EQ(decision(batch).target_notional_usd.at("A"), 250);
    ASSERT_EQ(observed.size(), 1u);
    ASSERT_TRUE(observed[0].volatility);
    EXPECT_DOUBLE_EQ(observed[0].sized_weights.get("A"), 0.5);
    EXPECT_DOUBLE_EQ(observed[0].approved_weights.get("A"), 0.25);
    EXPECT_DOUBLE_EQ(observed[0].volatility->raw_signal_volatility, 0.5);
    EXPECT_DOUBLE_EQ(observed[0].volatility->scaling_factor, 0.5);
    EXPECT_DOUBLE_EQ(observed[0].volatility->pre_constraint_volatility, 0.25);
    EXPECT_DOUBLE_EQ(observed[0].volatility->post_constraint_volatility, 0.125);
}

TEST(PortfolioRisk, R18_RequestingObservationsCannotChangeApprovedEconomicIntent)
{
    auto quiet = makeEngine({.full_weight = 0.8, .asset_cap = 0.25});
    auto observedEngine = makeEngine({.full_weight = 0.8, .asset_cap = 0.25});
    const auto plain = quiet.onSignals(signals(), marks(), account());
    std::vector<PortfolioRiskEvaluation> observed;
    const auto withObservation = observedEngine.onSignals(signals(), marks(), account(), &observed);
    EXPECT_EQ(decision(plain).target_notional_usd, decision(withObservation).target_notional_usd);
    EXPECT_DOUBLE_EQ(decision(plain).target_notional_usd.at("A"), 250);
    EXPECT_EQ(decision(plain).decisions.at("A").action,
              decision(withObservation).decisions.at("A").action);
}

TEST(PortfolioRisk, R18_FutureMarksCannotChangeCurrentEquityOrTargets)
{
    auto data = marks();
    auto snapshot = account(800);
    snapshot.positions = {{"A", 2}};
    auto baselineEngine = makeEngine();
    const auto baseline = baselineEngine.onSignals(signals(), data, snapshot);
    data[20240104]["A"].close = 1000000;
    auto withFutureEngine = makeEngine();
    const auto withFuture = withFutureEngine.onSignals(signals(), data, snapshot);
    EXPECT_DOUBLE_EQ(decision(withFuture).reference_capital, 1000);
    EXPECT_DOUBLE_EQ(decision(withFuture).target_notional_usd.at("A"), 250);
    EXPECT_EQ(decision(baseline).target_notional_usd, decision(withFuture).target_notional_usd);
}

TEST(PortfolioRisk, R14_StaleOrFutureAccountCannotJoinTheCurrentSignalCycle)
{
    auto engine = makeEngine();
    for (Timestamp other : {20240102u, 20240104u}) {
        EXPECT_THROW(engine.onSignals(signals(), marks(), account(1000, other)), std::invalid_argument);
        EXPECT_EQ(engine.lastTimestamp(), 0u);
    }
    const auto retry = engine.onSignals(signals(), marks(), account());
    EXPECT_DOUBLE_EQ(decision(retry).target_notional_usd.at("A"), 250);
}

TEST(PortfolioRisk, R14_DuplicateOlderZeroAndUnavailableDatesCannotAdvanceTheFrontier)
{
    auto engine = makeEngine();
    engine.onSignals(signals(), marks(), account());
    EXPECT_THROW(engine.onSignals(signals(), marks(), account()), std::logic_error);
    EXPECT_THROW(engine.onSignals(signals({{"A", 1}}, 20240102), marks(), account(1000, 20240102)),
                 std::logic_error);
    EXPECT_THROW(engine.onSignals(signals({{"A", 1}}, 0), marks(), account(1000, 0)),
                 std::invalid_argument);
    EXPECT_THROW(engine.onSignals(signals({{"A", 1}}, 20240104), marks(), account(1000, 20240104)),
                 std::out_of_range);
    EXPECT_EQ(engine.lastTimestamp(), date);
}

TEST(PortfolioRisk, R14_InvalidSignalIdentityMembershipAndValuesCannotProduceTargets)
{
    auto duplicate = signals();
    duplicate.strategies.push_back(duplicate.strategies.front());
    auto missing = signals();
    missing.strategies.clear();
    auto unknown = signals();
    unknown.strategies[0].strategy_id = 9;
    auto renamed = signals();
    renamed.strategies[0].strategy_name = "Renamed";
    const std::vector<StrategyIntentBatch> invalid{duplicate, missing, unknown, renamed,
        signals({{"", 1}}), signals({{"A", 2}}),
        signals({{"A", std::numeric_limits<double>::infinity()}})};
    for (const auto& input : invalid) {
        auto engine = makeEngine();
        EXPECT_ANY_THROW(engine.onSignals(input, marks(), account()));
        EXPECT_EQ(engine.lastTimestamp(), 0u);
    }
}

TEST(PortfolioRisk, R14_InvalidStrategyConfigurationCannotStartRiskProcessing)
{
    EXPECT_THROW(makeConfig({.id = 0}), std::invalid_argument);
    for (double bad : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()})
        EXPECT_THROW(makeConfig({.allocation = bad}), std::invalid_argument);
    EXPECT_THROW((PortfolioRiskStrategyConfig{7, "", 1, std::make_unique<EqualWeightSizer>(0.25),
        RiskConstraints(1, 1), std::make_unique<EntryExitOnlyRebalancePolicy>()}), std::invalid_argument);
    EXPECT_THROW((PortfolioRiskStrategyConfig{7, "Pure_RSI", 1, nullptr,
        RiskConstraints(1, 1), std::make_unique<EntryExitOnlyRebalancePolicy>()}), std::invalid_argument);
    EXPECT_THROW((PortfolioRiskStrategyConfig{7, "Pure_RSI", 1,
        std::make_unique<EqualWeightSizer>(0.25), RiskConstraints(1, 1), nullptr}), std::invalid_argument);
    EXPECT_THROW((PortfolioRiskEngine{std::vector<PortfolioRiskStrategyConfig>{}}), std::invalid_argument);
    std::vector<PortfolioRiskStrategyConfig> duplicate;
    duplicate.push_back(makeConfig());
    duplicate.push_back(makeConfig());
    EXPECT_THROW((PortfolioRiskEngine{std::move(duplicate)}), std::invalid_argument);
}

TEST(PortfolioRisk, R14_InvalidAccountMoneyPositionsOrEquityCannotFundTargets)
{
    std::vector<AccountSnapshot> invalid;
    auto bad = account();
    bad.cash = std::numeric_limits<double>::quiet_NaN();
    invalid.push_back(bad);
    bad = account();
    bad.collateral_usd = -1;
    invalid.push_back(bad);
    bad.collateral_usd = std::numeric_limits<double>::infinity();
    invalid.push_back(bad);
    bad = account();
    bad.positions = {{"A", std::numeric_limits<double>::infinity()}};
    invalid.push_back(bad);
    invalid.push_back(account(0));
    invalid.push_back(account(-1));
    for (const auto& snapshot : invalid) {
        auto engine = makeEngine();
        EXPECT_ANY_THROW(engine.onSignals(signals(), marks(), snapshot));
        EXPECT_EQ(engine.lastTimestamp(), 0u);
    }
    for (double invalidClose : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
        auto data = marks();
        data.at(date).at("A").close = invalidClose;
        auto snapshot = account(900);
        snapshot.positions = {{"A", 1}};
        auto engine = makeEngine();
        EXPECT_ANY_THROW(engine.onSignals(signals(), data, snapshot));
    }
}

TEST(PortfolioRisk, R14_MissingOrNonfiniteStrategyHoldingsCannotBeTreatedAsFlat)
{
    auto missing = account();
    missing.strategy_positions.clear();
    auto invalid = account();
    invalid.strategy_positions.at(7) = {{"A", std::numeric_limits<double>::infinity()}};
    for (const auto& snapshot : {missing, invalid}) {
        auto engine = makeEngine();
        EXPECT_THROW(engine.onSignals(signals(), marks(), snapshot), std::invalid_argument);
        EXPECT_EQ(engine.lastTimestamp(), 0u);
    }
}

TEST(PortfolioRisk, R14_MissingCurrentHeldPriceCannotFallBackToAFutureMark)
{
    auto data = marks();
    data.at(date).erase("A");
    data[20240104]["A"].close = 1000000;
    auto snapshot = account(900);
    snapshot.positions = {{"A", 1}};
    auto engine = makeEngine();
    EXPECT_THROW(engine.onSignals(signals(), data, snapshot), std::runtime_error);
    EXPECT_EQ(engine.lastTimestamp(), 0u);
    data.at(date).emplace("A", marks().at(date).at("A"));
    const auto retry = engine.onSignals(signals(), data, snapshot);
    EXPECT_DOUBLE_EQ(decision(retry).target_notional_usd.at("A"), 250);
}

TEST(PortfolioRisk, R14_RestoredFrontierPreventsReplayAndAllowsTheNextCompletedCycle)
{
    auto engine = makeEngine();
    engine.restoreLastTimestamp(date);
    engine.restoreLastTimestamp(date);
    EXPECT_THROW(engine.restoreLastTimestamp(20240102), std::logic_error);
    EXPECT_THROW(engine.onSignals(signals(), marks(), account()), std::logic_error);
    auto data = marks();
    data[20240104] = data.at(date);
    const auto next = engine.onSignals(signals({{"A", 1}}, 20240104), data, account(1000, 20240104));
    EXPECT_EQ(next.decision_timestamp, 20240104u);
    EXPECT_DOUBLE_EQ(decision(next).target_notional_usd.at("A"), 250);
}

TEST(PortfolioRisk, R12_NegativeCashCanBeFundedByPositiveSignedPositionEquity)
{
    auto snapshot = account(-100);
    snapshot.positions = {{"A", 2}};
    auto engine = makeEngine();
    const auto batch = engine.onSignals(signals(), marks(), snapshot);
    EXPECT_DOUBLE_EQ(decision(batch).reference_capital, 100);
    EXPECT_DOUBLE_EQ(decision(batch).target_notional_usd.at("A"), 25);
}

} // namespace
