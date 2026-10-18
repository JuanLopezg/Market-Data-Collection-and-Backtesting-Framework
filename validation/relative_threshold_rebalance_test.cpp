// RelativeThresholdRebalancePolicy (user rule 2026-10-15): a held position is resized only
// when its target weight differs from its current weight by at least 20% of that weight.
// Entries, exits and side reversals always happen.
#include <cassert>
#include <iostream>
#include <stdexcept>

#include "relative_threshold_rebalance_policy.h"

namespace {

// Current weight = quantity * price / capital: 10 units at 100 with 10,000 capital = 10%.
constexpr double capital = 10000.0;
constexpr double price = 100.0;

bool resizes(const RelativeThresholdRebalancePolicy& policy, double desired, double quantity)
{
    const RebalanceDecision decision = policy.decide(1.0, desired, quantity, price, capital);
    assert(decision.action == RebalanceAction::TargetWeight || decision.action == RebalanceAction::Hold);
    return decision.action == RebalanceAction::TargetWeight;
}

} // namespace

int main()
{
    const RelativeThresholdRebalancePolicy policy(0.20);

    // Long at 10%: holds strictly inside (8%, 12%), resizes outside.
    assert(!resizes(policy, 0.1199, 10.0));
    assert(!resizes(policy, 0.0801, 10.0));
    assert(!resizes(policy, 0.10, 10.0));
    assert(resizes(policy, 0.1201, 10.0));
    assert(resizes(policy, 0.0799, 10.0));
    assert(policy.decide(1.0, 0.13, 10.0, price, capital).target_weight == 0.13);

    // The band scales with the weight: at 5% it is (4%, 6%), not +-2 points.
    assert(!resizes(policy, 0.059, 5.0));
    assert(resizes(policy, 0.061, 5.0));
    assert(!resizes(policy, 0.041, 5.0));
    assert(resizes(policy, 0.039, 5.0));

    // Shorts use the size of the weight: -10% holds for -11%, resizes for -13%.
    assert(!resizes(policy, -0.11, -10.0));
    assert(resizes(policy, -0.13, -10.0));

    // The current weight follows the price: 10 units at 120 are 12%; a 10% target is
    // 16.7% away, so it holds.
    assert(policy.decide(1.0, 0.10, 10.0, 120.0, capital).action == RebalanceAction::Hold);

    // Entries, exits and reversals ignore the threshold.
    assert(policy.decide(1.0, 0.10, 0.0, price, capital).action == RebalanceAction::TargetWeight);
    assert(policy.decide(0.0, 0.10, 10.0, price, capital).action == RebalanceAction::Flat);
    assert(policy.decide(1.0, 0.0, 10.0, price, capital).action == RebalanceAction::Flat);
    assert(policy.decide(0.0, 0.0, 0.0, price, capital).action == RebalanceAction::Hold);
    assert(policy.decide(-1.0, -0.10, 10.0, price, capital).action == RebalanceAction::TargetWeight);

    // Zero threshold resizes on any change; invalid inputs are refused.
    const RelativeThresholdRebalancePolicy always(0.0);
    assert(resizes(always, 0.1001, 10.0));
    bool refused = false;
    try { RelativeThresholdRebalancePolicy bad(-0.1); } catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
    refused = false;
    try { policy.decide(1.0, 0.10, 10.0, price, 0.0); } catch (const std::invalid_argument&) { refused = true; }
    assert(refused);

    std::cout << "relative_threshold_rebalance_test: PASS\n";
    return 0;
}
