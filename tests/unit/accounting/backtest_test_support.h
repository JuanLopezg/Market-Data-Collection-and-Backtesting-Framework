#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "accounting_test_support.h"
#include "backtester.h"
#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"

namespace accounting_test {

using Schedule = std::map<Timestamp, double>;

// A deterministic source of close-time opinions, with no sizing/execution/PnL.
// The real StrategyInstance, sizer, risk, policy and runtime handle everything else.
class ScheduledSignal final : public Strategy {
public:
    explicit ScheduledSignal(Schedule changes)
        : Strategy("Scheduled", 1, nullptr, nullptr, 1), changes_(std::move(changes)) {}

    void updateSignals(const MarketData& data, Timestamp at, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        (void)indicators;
        const auto current = data.find(at);
        if (current == data.end() || !current->second.contains("BTC"))
            throw std::runtime_error("Synthetic signal requires the current BTC bar");
        const auto change = changes_.find(at);
        if (change != changes_.end()) signals.set("BTC", change->second);
    }

    std::vector<IndicatorSpec> requiredIndicators() const override { return {}; }

private:
    Schedule changes_;
};

inline StrategyInstance strategy(StrategyID id, double allocation, double weight,
                                 Schedule changes)
{
    return StrategyInstance(id, std::make_unique<ScheduledSignal>(std::move(changes)),
        allocation, std::make_unique<EqualWeightSizer>(weight), RiskConstraints(1.0, 1.0),
        std::make_unique<EntryExitOnlyRebalancePolicy>());
}

inline StrategyPortfolio oneStrategy(Schedule changes = {{kD1, 1.0}, {kD2, 0.0}})
{
    StrategyPortfolio result;
    result.push_back(strategy(kAlpha, 1.0, 0.5, std::move(changes)));
    return result;
}

inline OHLCV bar(double open, double close)
{
    return {open, std::max(open, close) + 10.0, std::min(open, close) - 10.0,
            close, 10000.0};
}

inline OHLCVData market(std::size_t days = 3)
{
    OHLCVData data;
    if (days >= 1) data.data["BTC"][kD1] = bar(80.0, 100.0);
    if (days >= 2) data.data["BTC"][kD2] = bar(125.0, 150.0);
    if (days >= 3) data.data["BTC"][kD3] = bar(140.0, 130.0);
    return data;
}

} // namespace accounting_test
