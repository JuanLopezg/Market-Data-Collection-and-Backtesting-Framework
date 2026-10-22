#pragma once

#include "catalog_definitions.h"
#include "proposal_research_strategies.h"

// Strategies proposed to the user on 2026-10-22 (algotrading_research_html --proposals).
// Sizing comes from the command line (--sizer equal-weight with quantityPercent below, or
// --sizer volatility-target).
namespace research_studies {

inline std::vector<StrategyDefinition> makeProposalDefinitions(const std::string& benchmark)
{
    std::vector<StrategyDefinition> definitions;
    auto add = [&](std::string name, double quantityPercent, std::function<std::unique_ptr<Strategy>()> create) {
        definitions.push_back(StrategyDefinition{
            std::move(name), {{"quantityPercent", quantityPercent}}, {},
            [create](const ParameterValues&) { return create(); },
            [](const ParameterValues&) { return true; }});
    };
    // 1. PureRSI as decided for LIVE: RSI(8) > 80 / < 65, top 20, ranked by SMA(volume, 25).
    add("P1_PureRSI_8_80_65", 10.0, [] {
        return std::make_unique<StrategyPureRSI>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(),
                                                 CATALOG_MAX_RANK, 8, 80.0, 65.0);
    });
    // 2. Donchian 20/50/100 ensemble on the top 20, up to 20 positions.
    add("P2_Donchian_Ensemble", 5.0, [] {
        return std::make_unique<DonchianEnsembleStrategy>(20, makeTopLiquidityUniverse(), catalogVolumeRanker(),
                                                          CATALOG_MAX_RANK);
    });
    // 3. SMA(10) > SMA(40) trend on BTC and ETH, 50% each.
    add("P3_SMA_Trend_BTC_ETH", 50.0, [] {
        return std::make_unique<FixedListSmaTrendStrategy>(std::set<Coin>{"BTCUSDT", "ETHUSDT"}, 10, 40);
    });
    // 4. Long-only weekly momentum: top 5 by ROC(7) of the top 20 while BTC > SMA(50),
    //    20% close-based stop, 20% each.
    add("P4_Momentum_Long_Weekly", 20.0, [benchmark] {
        return std::make_unique<WeeklyMomentumLongStrategy>(5, makeTopLiquidityUniverse(), 7, 7, 0.20, 50, benchmark);
    });
    return definitions;
}

} // namespace research_studies
