#pragma once

#include <vector>

#include "target_portfolio.h"


/**************************************************************************************
 * Type    : PortfolioAggregator
 * Purpose : Net independently-sized strategy target portfolios asset by asset
 *
 * No additional risk layer is applied here. Each strategy has already completed its own
 * sizing/risk/rebalance process before reaching this boundary.
 **************************************************************************************/
class PortfolioAggregator {
public:
    TargetPortfolio aggregate(const std::vector<TargetPortfolio>& strategyTargets) const
    {
        TargetPortfolio result;

        for (const TargetPortfolio& target : strategyTargets) {
            for (const auto& [coin, exposure] : target.values())
                result.add(coin, exposure);
        }

        return result;
    }
};
