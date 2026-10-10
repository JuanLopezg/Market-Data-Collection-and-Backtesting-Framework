#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "portfolio_risk_engine.h"
#include "volatility_target_sizer.h"

class FixtureCovariance final : public CovarianceEstimator {
    bool missing_;
    bool fail_diagnostics_;
    mutable int calls_ = 0;
public:
    explicit FixtureCovariance(bool missing = false, bool failDiagnostics = false)
        : missing_(missing), fail_diagnostics_(failDiagnostics) {}
    std::optional<CovarianceMatrix> estimate(const MarketData&, Timestamp timestamp, const std::vector<Coin>&) const override {
        assert(timestamp == 20261007);
        if (missing_) return std::nullopt;
        if (++calls_ > 1 && fail_diagnostics_) throw std::runtime_error("Fixture diagnostic unavailable");
        return CovarianceMatrix({"BTC", "ETH"}, {{0.04, 0.0}, {0.0, 0.09}});
    }
};

PortfolioRiskEngine engine(std::unique_ptr<PortfolioSizer> sizer, double assetCap = 0.25) {
    std::vector<PortfolioRiskStrategyConfig> configurations;
    configurations.emplace_back(1, "Fixture", 1.0, std::move(sizer), RiskConstraints(0.4, assetCap), std::make_unique<EntryExitOnlyRebalancePolicy>());
    return PortfolioRiskEngine(std::move(configurations));
}

void near(double actual, double expected) { assert(std::abs(actual - expected) < 1e-12); }

int main() {
    const Timestamp timestamp = 20261007;
    MarketData market;
    market[timestamp]["BTC"].close = 10;
    market[timestamp]["ETH"].close = 10;
    market[20261008]["BTC"].close = 999999; // Future prices must not enter capital or diagnostics.
    AccountSnapshot account;
    account.timestamp = timestamp;
    account.cash = 1000;
    account.positions["ETH"] = -2;
    account.strategy_positions[1]["ETH"] = -2;
    StrategyIntentBatch signals;
    signals.timestamp = timestamp;
    signals.strategies.push_back({1, "Fixture", {{"BTC", 1.0}, {"ETH", -1.0}}});
    std::vector<PortfolioRiskEvaluation> evaluations;
    auto observed = engine(std::make_unique<EqualWeightSizer>(0.5));
    auto unchanged = engine(std::make_unique<EqualWeightSizer>(0.5));
    const auto decision = observed.onSignals(signals, market, account, &evaluations);
    const auto reference = unchanged.onSignals(signals, market, account);
    assert(decision.strategies[0].target_notional_usd == reference.strategies[0].target_notional_usd);
    assert(decision.strategies[0].decisions.size() == 1); // ETH remains HOLD, not a capped resize.
    assert(evaluations.size() == 1 && evaluations[0].sizing_available);
    const auto& evaluation = evaluations[0];
    near(evaluation.reference_capital, 980);
    near(evaluation.sized_weights.get("BTC"), 0.5);
    near(evaluation.sized_weights.get("ETH"), -0.5);
    near(evaluation.constraints.asset_capped_weights.get("BTC"), 0.25);
    near(evaluation.constraints.gross_after_asset_cap, 0.5);
    near(evaluation.constraints.gross_scale, 0.8);
    near(evaluation.approved_weights.get("ETH"), -0.2);
    assert(evaluation.rebalance_decisions.at("ETH").action == RebalanceAction::Hold);
    assert(evaluation.volatility_state == "NOT_APPLICABLE");
    near(decision.strategies[0].target_notional_usd.at("BTC"), 196);

    auto zeroCap = engine(std::make_unique<EqualWeightSizer>(0.5), 0);
    const auto flattened = zeroCap.onSignals(signals, market, account, &evaluations);
    assert(flattened.strategies[0].target_notional_usd.at("ETH") == 0);
    assert(evaluations[0].approved_weights.values().empty());
    assert(evaluations[0].rebalance_decisions.at("BTC").action == RebalanceAction::Hold);

    auto volatility = engine(std::make_unique<VolatilityTargetSizer>(std::make_unique<FixtureCovariance>(), 0.2));
    volatility.onSignals(signals, market, account, &evaluations);
    assert(evaluations[0].volatility_state == "OBSERVED");
    near(evaluations[0].volatility->raw_signal_volatility, std::sqrt(0.13));
    near(evaluations[0].volatility->pre_constraint_volatility, 0.2);
    near(evaluations[0].volatility->post_constraint_volatility, std::sqrt(0.0052));

    auto missing = engine(std::make_unique<VolatilityTargetSizer>(std::make_unique<FixtureCovariance>(true), 0.2));
    assert(missing.onSignals(signals, market, account, &evaluations).strategies.empty());
    assert(!evaluations[0].sizing_available && evaluations[0].rebalance_decisions.empty());

    auto unavailable = engine(std::make_unique<VolatilityTargetSizer>(std::make_unique<FixtureCovariance>(false, true), 0.2));
    assert(!unavailable.onSignals(signals, market, account, &evaluations).strategies.empty());
    assert(evaluations[0].volatility_state == "DIAGNOSTIC_FAILED" && !evaluations[0].volatility);
    std::cout << "PORTFOLIO RISK DIAGNOSTICS: PASS: caps, signed weights, HOLD/FLAT, sizing availability, volatility and unchanged decisions\n";
}
