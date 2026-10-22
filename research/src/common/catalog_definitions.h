#pragma once

#include "catalog_research_strategies.h"
#include "research_strategy_definitions.h"

// The user's RealTest strategy catalog (2026-10-22), one plain variant each (the first one
// listed for that strategy). CanTrade = top 20 by SMA(volume, 25) (the Binance perpetual CSV
// volume is quote volume); RV = rank by SMA(volume, 25), highest first. A strategy whose first
// variant has no SetupScore is ranked by RV. Quantity 100/10 = 10% per signal (Follow_BTC:
// 100/15 with 15 positions). Used by algotrading_research_html --catalog.
namespace research_studies {

// Every ranked candidate may enter (no ranking-position cap).
inline constexpr unsigned int CATALOG_MAX_RANK = 1000000;

inline std::unique_ptr<Ranker> catalogVolumeRanker()
{
    return std::make_unique<IndicatorRanker>(IndicatorSpec{IndicatorKind::SMA, PriceField::Volume, 25}, true);
}

inline std::unique_ptr<Ranker> catalogRocRanker(unsigned int length, bool descending = true)
{
    return std::make_unique<IndicatorRanker>(IndicatorSpec{IndicatorKind::ROC, PriceField::Close, length}, descending);
}

inline std::vector<StrategyDefinition> makeCatalogDefinitions(const std::string& benchmark)
{
    std::vector<StrategyDefinition> definitions;
    auto add = [&](std::string name, double quantityPercent, std::function<std::unique_ptr<Strategy>()> create) {
        definitions.push_back(StrategyDefinition{
            std::move(name), {{"quantityPercent", quantityPercent}}, {},
            [create](const ParameterValues&) { return create(); },
            [](const ParameterValues&) { return true; }});
    };

    // ATH_Chaser: FromATH 0.15, ExitFromEntry 0.10 (the first variant's value is garbled;
    // 0.10 as in the second), SetupScore ROC(C, 30).
    add("ATH_Chaser", 10.0, [] {
        return std::make_unique<ATHChaserStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(30), CATALOG_MAX_RANK, 0.15, 0.10);
    });
    // ATR_Breakout: stop at H + 0.75 x ATR(3), exit after 2 bars.
    add("ATR_Breakout", 10.0, [] {
        return std::make_unique<ATRBreakoutStrategy>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK, 2, 0.75, 3);
    });
    // xH_Breakout: BTC > MA(50), stop at Highest(H, 15), exit after 15 bars, SetupScore RV.
    add("xH_Breakout", 10.0, [benchmark] {
        return std::make_unique<XHBreakoutTimedStrategy>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK,
                                                         15, 15, 50, benchmark);
    });
    // MRShort: RSI(5) > 70, BTC < MA(50), short stop at C - 0.3 x ATR(5), exit after 3 bars,
    // SetupScore RV. Its protective stop is close + ATR (ShortMeanReversionStrategy), not H[BarsHeld].
    add("MRShort", 10.0, [benchmark] {
        return std::make_unique<ShortMeanReversionStrategy>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK,
                                                            5, 70.0, 50, 0.30, 5, 3, benchmark);
    });
    // MR_LowsSweeper: BTC > MA(50), limit at Lowest(L, 2), exit after 15 bars, SetupScore ROC(C, 7).
    add("MR_LowsSweeper", 10.0, [benchmark] {
        return std::make_unique<LowsSweeperStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(7), CATALOG_MAX_RANK,
                                                     2, 15, 50, benchmark);
    });
    // Pure_Mom: BTC > MA(50), SetupScore ROC(C, 7), exit after 7 bars.
    add("Pure_Mom", 10.0, [benchmark] {
        return std::make_unique<PureMomentumStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(7), CATALOG_MAX_RANK,
                                                      7, 50, benchmark);
    });
    // Pure_RSI: RSI(7) > 80, exit RSI(7) < 70, SetupScore RV.
    add("Pure_RSI", 10.0, [] {
        return std::make_unique<StrategyPureRSI>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK, 7, 80.0, 70.0);
    });
    // MA_Cross: MA(C, 10) > MA(C, 20) and BTC > MA(50); exit MA(10) < MA(20); RV.
    add("MA_Cross", 10.0, [benchmark] {
        return std::make_unique<MACrossStrategy>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK,
                                                 10, 20, 50, benchmark);
    });
    // MA_Simple: C > MA(C, 20) and BTC > MA(50); exit C < MA(20); RV.
    add("MA_Simple", 10.0, [benchmark] {
        return std::make_unique<MASimpleStrategy>(10, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK,
                                                  20, 50, benchmark);
    });
    // Follow_BTC: hold while BTC > MA(C, 50) (len 50 of the listed values), 15 positions, 100/15.
    add("Follow_BTC", 100.0 / 15.0, [benchmark] {
        return std::make_unique<FollowBenchmarkStrategy>(15, makeTopLiquidityUniverse(), catalogVolumeRanker(), CATALOG_MAX_RANK,
                                                         50, benchmark);
    });
    // MR_ATRLimit: BTC > MA(50), limit at C - 0.75 x ATR(3), exit after 2 bars, SetupScore ROC(C, 10).
    add("MR_ATRLimit", 10.0, [benchmark] {
        return std::make_unique<ATRLimitStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(10), CATALOG_MAX_RANK,
                                                  3, 0.75, 2, 50, benchmark);
    });
    // MR_RSILong: RSI(3) < 10, exit after 1 bar, SetupScore ROC(C, 30).
    add("MR_RSILong", 10.0, [] {
        return std::make_unique<RSIMeanReversionStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(30), CATALOG_MAX_RANK,
                                                          3, 10.0, 1);
    });
    // BargainCatcher: C / C[1] - 1 < -10% and C > MA(C, 50), exit after 2 bars; no score, the
    // largest fall first (ROC(1) ascending, as in the earlier BargainChaser studies).
    add("BargainCatcher", 10.0, [] {
        return std::make_unique<BargainChaserStrategy>(10, makeTopLiquidityUniverse(), catalogRocRanker(1, false), CATALOG_MAX_RANK,
                                                       2, 10.0, 50);
    });
    return definitions;
}

} // namespace research_studies
