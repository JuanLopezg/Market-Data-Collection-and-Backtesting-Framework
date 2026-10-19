#include <gtest/gtest.h>

#include "portfolio_volatility_estimator.h"
#include "risk_test_data.h"
#include "volatility_target.h"

#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace risk_test;

TEST(RiskSizing, R1_EqualWeightScalesSignalStrengthWithoutNormalizingByActiveCount)
{
    const EqualWeightSizer sizer(0.25);
    auto opinions = signalState({{"A", 1}, {"B", 0.5}, {"C", -0.25}});
    const auto sized = sizer.size(opinions, {}, date);
    ASSERT_TRUE(sized);
    EXPECT_DOUBLE_EQ(sized->get("A"), 0.25);
    EXPECT_DOUBLE_EQ(sized->get("B"), 0.125);
    EXPECT_DOUBLE_EQ(sized->get("C"), -0.0625);
    opinions.set("D", 1);
    const auto expanded = sizer.size(opinions, {}, date);
    ASSERT_TRUE(expanded);
    EXPECT_DOUBLE_EQ(expanded->get("A"), 0.25);
    EXPECT_DOUBLE_EQ(expanded->get("B"), 0.125);
}

TEST(RiskSizing, R1_EmptySignalsOrZeroFullWeightAreAvailableEmptyTargets)
{
    const auto empty = EqualWeightSizer(0.25).size(SignalState{}, {}, date);
    const auto disabled = EqualWeightSizer(0).size(signalState({{"A", 1}}), {}, date);
    ASSERT_TRUE(empty);
    ASSERT_TRUE(disabled);
    EXPECT_TRUE(empty->values().empty());
    EXPECT_TRUE(disabled->values().empty());
}

TEST(RiskCovariance, R2_SampleDenominatorAnnualizationAndCrossCovarianceMatchHandExample)
{
    const SampleCovarianceEstimator estimator(2, 2);
    const auto covariance = estimator.estimate(volatilityHistory(), date, {"B", "A"});
    ASSERT_TRUE(covariance);
    EXPECT_DOUBLE_EQ(covariance->get("A", "A"), 0.25);
    EXPECT_DOUBLE_EQ(covariance->get("A", "B"), -0.5);
    EXPECT_DOUBLE_EQ(covariance->get("B", "A"), -0.5);
    EXPECT_DOUBLE_EQ(covariance->get("B", "B"), 1);
}

TEST(RiskCovariance, R3_OnlyTheLatestRequestedNumberOfValidReturnsContributes)
{
    const auto data = market({{"A", {50, 100, 125, 93.75}}});
    const auto covariance = SampleCovarianceEstimator(2, 2).estimate(data, 20240104, {"A"});
    ASSERT_TRUE(covariance);
    EXPECT_DOUBLE_EQ(covariance->get("A", "A"), 0.25);
}

TEST(RiskCovariance, R3_FutureExtremeCannotChangeAnEarlierCovariance)
{
    auto data = volatilityHistory();
    const SampleCovarianceEstimator estimator(2, 2);
    const auto before = estimator.estimate(data, date, {"A", "B"});
    data[20240104]["A"].close = 1000000;
    data[20240104]["B"].close = 1;
    const auto after = estimator.estimate(data, date, {"A", "B"});
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_DOUBLE_EQ(after->get("A", "A"), 0.25);
    EXPECT_DOUBLE_EQ(after->get("A", "B"), before->get("A", "B"));
    EXPECT_DOUBLE_EQ(after->get("B", "B"), 1);
}

TEST(RiskCovariance, R3_MissingMiddleAssetCannotBeBridgedOrEstimatedPairwise)
{
    auto data = volatilityHistory();
    data.at(20240102).erase("B");
    EXPECT_FALSE(SampleCovarianceEstimator(2, 2).estimate(data, date, {"A", "B"}));
}

TEST(RiskCovariance, R3_InvalidClosesAndInsufficientHistoryDeferTheEstimate)
{
    const SampleCovarianceEstimator estimator(2, 2);
    EXPECT_FALSE(estimator.estimate(market({{"A", {100, 125}}}), 20240102, {"A"}));
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        auto data = volatilityHistory();
        data.at(20240102).at("A").close = invalid;
        EXPECT_FALSE(estimator.estimate(data, date, {"A"}));
    }
}

TEST(RiskCovariance, R3_DuplicateAssetsAndInvalidEstimatorConfigurationAreRejected)
{
    EXPECT_THROW((SampleCovarianceEstimator{1, 2}), std::invalid_argument);
    EXPECT_THROW((SampleCovarianceEstimator{2, 0}), std::invalid_argument);
    EXPECT_THROW((SampleCovarianceEstimator{2, std::numeric_limits<double>::infinity()}),
                 std::invalid_argument);
    EXPECT_THROW(SampleCovarianceEstimator(2, 2).estimate(volatilityHistory(), date, {"A", "A"}),
                 std::invalid_argument);
}

TEST(RiskVolatility, R4_SignedWeightsRetainTheHedgeRatherThanAddingAbsoluteRisk)
{
    const CovarianceMatrix covariance({"A", "B"}, {{0.25, 0.5}, {0.5, 1}});
    const PortfolioVolatilityEstimator estimator;
    EXPECT_DOUBLE_EQ(estimator.estimate(weights({{"A", 1}, {"B", -0.5}}), covariance), 0);
    EXPECT_DOUBLE_EQ(estimator.estimate(weights({{"A", 1}, {"B", 0.5}}), covariance), 1);
    EXPECT_DOUBLE_EQ(estimator.estimate(TargetWeights{}, covariance), 0);
}

TEST(RiskVolatility, R4_InvalidVarianceOrUnknownCovarianceAssetCannotBeUsedForSizing)
{
    const PortfolioVolatilityEstimator estimator;
    EXPECT_THROW(estimator.estimate(weights({{"A", 1}}), CovarianceMatrix({"A"}, {{-1}})),
                 std::runtime_error);
    EXPECT_THROW(estimator.estimate(weights({{"MISSING", 1}}), CovarianceMatrix({"A"}, {{0.25}})),
                 std::invalid_argument);
}

TEST(RiskVolatility, R5_TargetScalingPreservesTheOriginalSignedSignalShape)
{
    const auto sized = VolatilityTarget(0.25).apply(weights({{"A", 1}, {"B", -0.5}}), 0.5);
    ASSERT_TRUE(sized);
    EXPECT_DOUBLE_EQ(sized->get("A"), 0.5);
    EXPECT_DOUBLE_EQ(sized->get("B"), -0.25);
}

TEST(RiskVolatility, R6_EmptyZeroTargetAndUnscalableZeroVolatilityStayDistinct)
{
    const auto empty = VolatilityTarget(0.25).apply(TargetWeights{}, 0);
    const auto disabled = VolatilityTarget(0).apply(weights({{"A", 1}}), 0);
    ASSERT_TRUE(empty);
    ASSERT_TRUE(disabled);
    EXPECT_TRUE(empty->values().empty());
    EXPECT_TRUE(disabled->values().empty());
    EXPECT_FALSE(VolatilityTarget(0.25).apply(weights({{"A", 1}}), 0));
}

TEST(RiskSizing, R5_RealCovarianceDeterminesVolatilityTargetWeightAndAnnualization)
{
    const auto opinions = signalState({{"A", 1}});
    const auto yearlyTwo = volSizer(0.25, 2)->size(opinions, volatilityHistory(), date);
    const auto yearlyEight = volSizer(0.25, 8)->size(opinions, volatilityHistory(), date);
    ASSERT_TRUE(yearlyTwo);
    ASSERT_TRUE(yearlyEight);
    EXPECT_DOUBLE_EQ(yearlyTwo->get("A"), 0.5);
    EXPECT_DOUBLE_EQ(yearlyEight->get("A"), 0.25);
}

TEST(RiskSizing, R5_MultipleSignalsAreScaledTogetherRatherThanIndividuallyEqualWeighted)
{
    const auto data = market({{"A", {100, 125, 93.75}}, {"B", {200, 250, 187.5}}});
    const auto sized = volSizer(0.375)->size(signalState({{"A", 1}, {"B", 0.5}}), data, date);
    ASSERT_TRUE(sized);
    EXPECT_DOUBLE_EQ(sized->get("A"), 0.5);
    EXPECT_DOUBLE_EQ(sized->get("B"), 0.25);
}

TEST(RiskSizing, R6_UnavailableCovarianceAndZeroVolatilityDeferActiveSizing)
{
    const auto opinions = signalState({{"A", 1}});
    EXPECT_FALSE(volSizer()->size(opinions, market({{"A", {100, 125}}}), 20240102));
    EXPECT_FALSE(volSizer()->size(opinions, market({{"A", {100, 100, 100}}}), date));
}

TEST(RiskSizing, R6_NoSignalsGiveAnAvailableEmptyTargetWithoutCovarianceHistory)
{
    const auto sized = volSizer()->size(SignalState{}, {}, date);
    ASSERT_TRUE(sized);
    EXPECT_TRUE(sized->values().empty());
}

TEST(RiskSizing, R6_ZeroVolatilityTargetDisablesExposureWithoutRequiringHistory)
{
    const auto sized = volSizer(0)->size(signalState({{"A", 1}}), {}, date);
    ASSERT_TRUE(sized);
    EXPECT_TRUE(sized->values().empty());
}

TEST(RiskCaps, R7_AssetCapsPrecedeProportionalGrossScalingAndPreserveSigns)
{
    RiskConstraintEvaluation observation;
    const auto approved = RiskConstraints(0.6, 0.5).apply(
        weights({{"A", 0.8}, {"B", -0.6}, {"C", 0.2}}), &observation);
    EXPECT_DOUBLE_EQ(observation.asset_capped_weights.get("A"), 0.5);
    EXPECT_DOUBLE_EQ(observation.asset_capped_weights.get("B"), -0.5);
    EXPECT_NEAR(observation.gross_after_asset_cap, 1.2, 1e-12);
    EXPECT_DOUBLE_EQ(observation.gross_scale, 0.5);
    EXPECT_DOUBLE_EQ(approved.get("A"), 0.25);
    EXPECT_DOUBLE_EQ(approved.get("B"), -0.25);
    EXPECT_NEAR(approved.get("C"), 0.1, 1e-12);
}

TEST(RiskCaps, R7_ZeroLimitsDisableExposureAndRepeatedConstraintsCannotAmplifyIt)
{
    const auto desired = weights({{"A", 0.8}, {"B", -0.6}, {"C", 0.2}});
    EXPECT_TRUE(RiskConstraints(0, 1).apply(desired).values().empty());
    EXPECT_TRUE(RiskConstraints(1, 0).apply(desired).values().empty());
    const RiskConstraints limits(0.6, 0.5);
    const auto once = limits.apply(desired);
    const auto twice = limits.apply(once);
    for (const auto& [coin, weight] : once.values()) {
        EXPECT_NEAR(twice.get(coin), weight, 1e-12);
        EXPECT_LE(std::abs(twice.get(coin)), std::abs(desired.get(coin)));
    }
}

TEST(RiskSizing, R7_InvalidFinancialConfigurationIsRejectedInsteadOfCreatingExposure)
{
    for (double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW((EqualWeightSizer{invalid}), std::invalid_argument);
        EXPECT_THROW((VolatilityTarget{invalid}), std::invalid_argument);
        EXPECT_THROW((RiskConstraints{invalid, 1}), std::invalid_argument);
        EXPECT_THROW((RiskConstraints{1, invalid}), std::invalid_argument);
    }
    EXPECT_THROW((VolatilityTargetSizer{nullptr, 0.25}), std::invalid_argument);
}

} // namespace
