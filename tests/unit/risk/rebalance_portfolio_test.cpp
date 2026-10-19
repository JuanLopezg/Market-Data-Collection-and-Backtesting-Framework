#include <gtest/gtest.h>

#include "manual_portfolio_risk.h"
#include "portfolio_targets.h"
#include "relative_threshold_rebalance_policy.h"
#include "risk_test_data.h"
#include "strategy_instance.h"
#include "target_decisions.h"
#include "threshold_rebalance_policy.h"
#include "validated/pure_rsi.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace risk_test;

TEST(MonetaryTargets, R8_ApprovedSignedWeightsReceiveCapitalExactlyOnce)
{
    const TargetPortfolioBuilder builder;
    const auto approved = weights({{"A", 0.25}, {"B", -0.125}});
    const auto target = builder.build(approved, 1000);
    EXPECT_DOUBLE_EQ(target.get("A"), 250);
    EXPECT_DOUBLE_EQ(target.get("B"), -125);
    EXPECT_TRUE(builder.build(approved, 0).values().empty());
    EXPECT_DOUBLE_EQ(approved.get("A"), 0.25);
}

TEST(MonetaryTargets, R8_IndependentPortfoliosNetByAssetWithoutMutatingTheirInputs)
{
    const TargetPortfolioBuilder builder;
    const auto first = builder.build(weights({{"A", 0.25}, {"B", -0.125}}), 1000);
    const auto second = builder.build(weights({{"A", -0.25}, {"C", 0.05}}), 1000);
    const PortfolioAggregator aggregator;
    const auto net = aggregator.aggregate({first, second});
    const auto reversed = aggregator.aggregate({second, first});
    EXPECT_FALSE(net.contains("A"));
    EXPECT_DOUBLE_EQ(net.get("B"), -125);
    EXPECT_DOUBLE_EQ(net.get("C"), 50);
    EXPECT_EQ(net.values(), reversed.values());
    EXPECT_DOUBLE_EQ(first.get("A"), 250);
    EXPECT_DOUBLE_EQ(second.get("A"), -250);
    EXPECT_DOUBLE_EQ(aggregator.aggregate({first, first}).get("A"), 500);
}

TEST(MonetaryTargets, R8_InvalidCapitalAndOverflowCannotBecomeExecutableNotionals)
{
    const TargetPortfolioBuilder builder;
    for (double capital : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()})
        EXPECT_THROW(builder.build(weights({{"A", 0.25}}), capital), std::invalid_argument);
    EXPECT_THROW(builder.build(weights({{"A", std::numeric_limits<double>::max()}}), 2),
                 std::invalid_argument);
}

TEST(RebalancePolicies, R9_NewActiveSignalsRequestSignedWeightsRatherThanQuantities)
{
    const EntryExitOnlyRebalancePolicy policy;
    const auto buy = policy.decide(1, 0.25, 0, 100, 1000);
    const auto sell = policy.decide(-1, -0.25, 0, 100, 1000);
    EXPECT_EQ(buy.action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(buy.target_weight, 0.25);
    EXPECT_EQ(sell.action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(sell.target_weight, -0.25);
    EXPECT_EQ(policy.decide(0, 0, 0, 100, 1000).action, RebalanceAction::Hold);
}

TEST(RebalancePolicies, R9_SameSideHoldsIgnoreDesiredWeightAndSignalStrengthChanges)
{
    const EntryExitOnlyRebalancePolicy policy;
    EXPECT_EQ(policy.decide(0.5, 0.125, 2, 100, 1000).action, RebalanceAction::Hold);
    EXPECT_EQ(policy.decide(1, 0.5, 2, 200, 1200).action, RebalanceAction::Hold);
    EXPECT_EQ(policy.decide(-1, -0.5, -3, 200, 1200).action, RebalanceAction::Hold);
}

TEST(RebalancePolicies, R9_ExitZeroApprovedExposureAndReversalAreExplicitActions)
{
    const EntryExitOnlyRebalancePolicy policy;
    EXPECT_EQ(policy.decide(0, 0, 2, 100, 1000).action, RebalanceAction::Flat);
    EXPECT_EQ(policy.decide(0, 0, -3, 100, 1000).action, RebalanceAction::Flat);
    EXPECT_EQ(policy.decide(1, 0, 2, 100, 1000).action, RebalanceAction::Flat);
    const auto reverse = policy.decide(-1, -0.25, 2, 100, 1000);
    EXPECT_EQ(reverse.action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(reverse.target_weight, -0.25);
}

TEST(RebalancePolicies, R10_AbsoluteThresholdUsesCapitalNormalizedDriftAndInclusiveBoundary)
{
    const ThresholdRebalancePolicy policy(0.125);
    EXPECT_EQ(policy.decide(1, 0.3125, 1, 250, 1000).action, RebalanceAction::Hold);
    const auto boundary = policy.decide(1, 0.375, 1, 250, 1000);
    EXPECT_EQ(boundary.action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(boundary.target_weight, 0.375);
}

TEST(RebalancePolicies, R11_RelativeThresholdScalesItsBandWithCurrentExposure)
{
    const RelativeThresholdRebalancePolicy policy(0.5);
    EXPECT_EQ(policy.decide(1, 0.3125, 1, 250, 1000).action, RebalanceAction::Hold);
    EXPECT_EQ(policy.decide(1, 0.375, 1, 250, 1000).action, RebalanceAction::TargetWeight);
    EXPECT_EQ(policy.decide(1, 0.125, 1, 250, 1000).action, RebalanceAction::TargetWeight);
    EXPECT_EQ(policy.decide(1, 0.15625, 0.5, 250, 1000).action, RebalanceAction::Hold);
    EXPECT_EQ(policy.decide(1, 0.1875, 0.5, 250, 1000).action, RebalanceAction::TargetWeight);
}

TEST(RebalancePolicies, R11_PublishedTwentyPercentRuleResizesAtTwelvePercentEquality)
{
    const RelativeThresholdRebalancePolicy policy(0.20);
    const auto decision = policy.decide(1, 0.12, 1, 100, 1000);
    EXPECT_EQ(decision.action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(decision.target_weight, 0.12);
}

TEST(RebalancePolicies, R10_EntriesExitsAndReversalsBypassLargeDriftThresholds)
{
    const ThresholdRebalancePolicy absolute(100);
    const RelativeThresholdRebalancePolicy relative(100);
    for (const RebalancePolicy* policy : {static_cast<const RebalancePolicy*>(&absolute),
                                         static_cast<const RebalancePolicy*>(&relative)}) {
        EXPECT_EQ(policy->decide(1, 0.25, 0, 100, 1000).action, RebalanceAction::TargetWeight);
        EXPECT_EQ(policy->decide(0, 0, 2, 100, 1000).action, RebalanceAction::Flat);
        EXPECT_EQ(policy->decide(-1, -0.25, 2, 100, 1000).action, RebalanceAction::TargetWeight);
    }
}

TEST(RebalancePolicies, R10_InvalidThresholdsOrHeldPositionValuationAreRejected)
{
    for (double bad : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW((ThresholdRebalancePolicy{bad}), std::invalid_argument);
        EXPECT_THROW((RelativeThresholdRebalancePolicy{bad}), std::invalid_argument);
    }
    const ThresholdRebalancePolicy absolute(0.125);
    const RelativeThresholdRebalancePolicy relative(0.5);
    for (const RebalancePolicy* policy : {static_cast<const RebalancePolicy*>(&absolute),
                                         static_cast<const RebalancePolicy*>(&relative)}) {
        EXPECT_THROW(policy->decide(1, 0.25, 2, 0, 1000), std::invalid_argument);
        EXPECT_THROW(policy->decide(1, 0.25, 2, 100, 0), std::invalid_argument);
        EXPECT_THROW(policy->decide(1, std::numeric_limits<double>::infinity(), 2, 100, 1000),
                     std::invalid_argument);
    }
}

TEST(TargetDecisions, R15_ExplicitTargetsReceiveCapitalAndOmittedHoldingsAreFlattened)
{
    auto intent = targetIntent();
    VirtualPositionState held;
    held.set("B", 2);
    ExecutionReferencePrices prices;
    prices.set("A", 100);
    prices.set("B", 50);
    setTargetDecisions(intent, {{"A", 0.25}}, &held, prices);
    EXPECT_EQ(intent.decisions.at("A").action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(intent.target_notional_usd.at("A"), 250);
    EXPECT_EQ(intent.decisions.at("B").action, RebalanceAction::Flat);
    EXPECT_DOUBLE_EQ(intent.target_notional_usd.at("B"), 0);
    EXPECT_DOUBLE_EQ(held.get("B"), 2);
    EXPECT_EQ(intent.decision_timestamp, date);
}

TEST(TargetDecisions, R15_MissingHeldPriceMeansHoldRatherThanAnInventedExitReference)
{
    auto intent = targetIntent();
    VirtualPositionState held;
    held.set("B", 2);
    ExecutionReferencePrices prices;
    prices.set("A", 100);
    setTargetDecisions(intent, {{"A", 0.25}}, &held, prices);
    EXPECT_FALSE(intent.decisions.contains("B"));
    EXPECT_FALSE(intent.target_notional_usd.contains("B"));
    EXPECT_DOUBLE_EQ(intent.target_notional_usd.at("A"), 250);
    EXPECT_DOUBLE_EQ(held.get("B"), 2);
}

PortfolioRiskEvaluation availableEvaluation()
{
    PortfolioRiskEvaluation evaluation;
    evaluation.strategy_id = 7;
    evaluation.strategy_name = "Pure_RSI";
    evaluation.reference_capital = 1000;
    evaluation.sizing_available = true;
    evaluation.approved_weights = weights({{"A", 0.25}});
    return evaluation;
}

TEST(TargetDecisions, R15_ManualResyncAddsAFormerlyAllHoldStrategyFromApprovedWeights)
{
    DecisionBatch approved;
    approved.decision_timestamp = date;
    StrategyPositionSnapshot held;
    held[7].set("A", 2);
    ExecutionReferencePrices prices;
    prices.set("A", 100);
    const auto resynced = resyncToApprovedTargets(approved, {availableEvaluation()}, held, prices);
    EXPECT_EQ(decision(resynced).decisions.at("A").action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(decision(resynced).target_notional_usd.at("A"), 250);
    EXPECT_EQ(decision(resynced).decision_timestamp, date);
    EXPECT_DOUBLE_EQ(held.at(7).get("A"), 2);
}

// FINDING: Resync changes an existing A FLAT/USD0 into TARGET(0.25)/USD250,
// contrary to the public promise to preserve existing non-HOLD decisions (spec R15).
TEST(TargetDecisions, DISABLED_R15_ManualResyncPreservesAnExplicitExitWhileResizingOtherHolds)
{
    DecisionBatch approved;
    approved.decision_timestamp = date;
    auto existing = targetIntent();
    existing.decisions.emplace("A", RebalanceDecision::flat());
    existing.target_notional_usd.emplace("A", 0);
    approved.strategies.push_back(existing);
    auto evaluation = availableEvaluation();
    evaluation.approved_weights.set("B", 0.5);
    StrategyPositionSnapshot held;
    held[7].set("A", 1);
    held[7].set("B", 2);
    ExecutionReferencePrices prices;
    prices.set("A", 100);
    prices.set("B", 50);
    const auto resynced = resyncToApprovedTargets(approved, {evaluation}, held, prices);
    EXPECT_EQ(decision(resynced).decisions.at("A").action, RebalanceAction::Flat);
    EXPECT_DOUBLE_EQ(decision(resynced).target_notional_usd.at("A"), 0);
    EXPECT_EQ(decision(resynced).decisions.at("B").action, RebalanceAction::TargetWeight);
    EXPECT_DOUBLE_EQ(decision(resynced).target_notional_usd.at("B"), 500);
    EXPECT_EQ(decision(approved).decisions.at("A").action, RebalanceAction::Flat);
}

TEST(TargetDecisions, R15_UnavailableSizingCannotCreateAResyncOrReplaceApprovedDecisions)
{
    DecisionBatch approved;
    approved.decision_timestamp = date;
    auto existing = targetIntent();
    existing.decisions.emplace("A", RebalanceDecision::flat());
    existing.target_notional_usd.emplace("A", 0);
    approved.strategies.push_back(existing);
    auto evaluation = availableEvaluation();
    evaluation.sizing_available = false;
    ExecutionReferencePrices prices;
    prices.set("A", 100);
    const auto retained = resyncToApprovedTargets(approved, {evaluation}, {}, prices);
    EXPECT_EQ(decision(retained).decisions.at("A").action, RebalanceAction::Flat);
    DecisionBatch allHold;
    allHold.decision_timestamp = date;
    EXPECT_TRUE(resyncToApprovedTargets(allHold, {evaluation}, {}, prices).strategies.empty());
}

ManualControl::ManualTargetIntent manualTarget(double weight)
{
    ManualControl::ManualTargetIntent request;
    request.request_id = "synthetic-request";
    request.correlation_id = "synthetic-cycle";
    request.actor = "unit-test";
    request.request_hash = "synthetic-hash";
    request.decision_timestamp = date;
    request.execution_timestamp = 20240104;
    request.asset_weights.emplace("BTCUSDT", weight);
    request.cash_weight = 1 - weight;
    return request;
}

TEST(ManualRisk, R16_ConfirmedAllocationIsRejectedRatherThanSilentlyClipped)
{
    const auto request = manualTarget(0.8);
    ExecutionReferencePrices prices;
    prices.set("BTCUSDT", 100);
    const auto result = ManualControl::ManualPortfolioRisk(1, 0.5).evaluate(request, 1000, {}, prices);
    EXPECT_FALSE(result.approved);
    EXPECT_EQ(result.reason, "RISK_CONSTRAINT_WOULD_MUTATE_REQUEST:BTCUSDT");
    EXPECT_TRUE(result.decisions.strategies.empty());
    EXPECT_DOUBLE_EQ(request.asset_weights.at("BTCUSDT"), 0.8);
}

TEST(ManualRisk, R16_ApprovedAllocationUsesExactCapitalAndFlattensOmittedManualHoldings)
{
    const auto request = manualTarget(0.25);
    ExecutionReferencePrices prices;
    prices.set("BTCUSDT", 100);
    prices.set("ETHUSDT", 50);
    StrategyPositionSnapshot held;
    held[ManualControl::kManualStrategyId].set("ETHUSDT", 3);
    const auto result = ManualControl::ManualPortfolioRisk().evaluate(request, 1000, held, prices);
    ASSERT_TRUE(result.approved) << result.reason;
    const auto& intent = decision(result.decisions, ManualControl::kManualStrategyId);
    EXPECT_DOUBLE_EQ(intent.target_notional_usd.at("BTCUSDT"), 250);
    EXPECT_EQ(intent.decisions.at("ETHUSDT").action, RebalanceAction::Flat);
    EXPECT_DOUBLE_EQ(intent.target_notional_usd.at("ETHUSDT"), 0);
    EXPECT_FALSE(intent.decisions.contains("CASH"));
    EXPECT_DOUBLE_EQ(held.at(ManualControl::kManualStrategyId).get("ETHUSDT"), 3);
}

TEST(InstanceRisk, R17_CappedEvaluationDoesNotResizeAnEntryExitOnlyHolding)
{
    StrategyInstance instance(7,
        std::make_unique<StrategyPureRSI>(2, nullptr, nullptr, 100, 2, 75, 25), 1,
        std::make_unique<EqualWeightSizer>(0.8), RiskConstraints(1, 0.1),
        std::make_unique<EntryExitOnlyRebalancePolicy>());
    instance.restoreState({{"A", 1}}, {{"A", 0.8}}, {{"A", 2}});
    const auto plan = instance.calculateRebalancePlan(marks(), date, 1000);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->get("A").action, RebalanceAction::Hold);
    EXPECT_TRUE(plan->values().empty());
    EXPECT_DOUBLE_EQ(instance.desiredWeights().get("A"), 0.1);
    EXPECT_DOUBLE_EQ(instance.virtualPositions().get("A"), 2);
}

TEST(InstanceRisk, R17_UnavailableSizingDefersThePlanWithoutDiscardingExistingState)
{
    StrategyInstance instance(7,
        std::make_unique<StrategyPureRSI>(2, nullptr, nullptr, 100, 2, 75, 25), 1,
        volSizer(), RiskConstraints(1, 1), std::make_unique<EntryExitOnlyRebalancePolicy>());
    instance.restoreState({{"A", 1}}, {{"A", 0.25}}, {{"A", 2}});
    const auto shortHistory = market({{"A", {100, 125}}});
    EXPECT_FALSE(instance.calculateRebalancePlan(shortHistory, 20240102, 1000));
    EXPECT_DOUBLE_EQ(instance.desiredWeights().get("A"), 0.25);
    EXPECT_DOUBLE_EQ(instance.virtualPositions().get("A"), 2);
}

} // namespace
