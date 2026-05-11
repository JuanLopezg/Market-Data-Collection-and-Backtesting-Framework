#include "strategy.h"

#include <utility>

#include "indicator_engine.h"
#include "logger.h"


Strategy::Strategy(
    std::string name,
    unsigned int maxPosOpen,
    double riskPerTradePctg,
    std::unique_ptr<UniverseSelector> universeSelector,
    std::unique_ptr<Ranker> ranker,
    double commissionEntryFactor,
    double commissionExitFactor,
    unsigned int maxRankingPosition
)
    : strategy_name_(std::move(name)),
      maxPosOpen_(maxPosOpen),
      riskPerTrade_(riskPerTradePctg),
      universeSelector_(std::move(universeSelector)),
      ranker_(std::move(ranker)),
      maxRankingPosition_(maxRankingPosition),
      commissionEntryFactor_(commissionEntryFactor),
      commissionExitFactor_(commissionExitFactor)
{
    if (!universeSelector_) {
        universeSelector_ = std::make_unique<AllUniverseSelector>();
    }
}


std::vector<IndicatorSpec> Strategy::requiredIndicators() const
{
    std::vector<IndicatorSpec> specs;

    if (universeSelector_) {
        const auto universeSpecs = universeSelector_->requiredIndicators();

        specs.insert(
            specs.end(),
            universeSpecs.begin(),
            universeSpecs.end()
        );
    }

    if (ranker_) {
        const auto rankerSpecs = ranker_->requiredIndicators();

        specs.insert(
            specs.end(),
            rankerSpecs.begin(),
            rankerSpecs.end()
        );
    }

    return specs;
}


/**********************************************************************************
 * Purpose : Execute strategy logic for a single timestamp
 **********************************************************************************/
void Strategy::calculateSignals(
    const MarketData& marketData,
    Timestamp ts,
    unsigned int& last_trade_id,
    std::vector<Trade>& current_trades,
    double strategy_allocation,
    bool live_trading,
    const IndicatorEngine& indicators
)
{
    const auto tsIt = marketData.find(ts);

    if (tsIt == marketData.end()) {
        LG_ERROR("No market data for timestamp {}", ts);
        return;
    }

    const auto& bars = tsIt->second;

    // ---------------------------------------------------------------------
    // 1. Update open trades
    // ---------------------------------------------------------------------
    unsigned int openCount = 0;

    for (auto& trade : current_trades) {
        if (trade.exited_) {
            LG_ERROR("Received a closed trade");
            continue;
        }

        const auto barIt = bars.find(trade.coin_);

        if (barIt == bars.end()) {
            LG_ERROR("No data for coin {}", trade.coin_);
            continue;
        }

        onBar(
            trade,
            trade.coin_,
            marketData,
            ts,
            live_trading,
            indicators
        );

        if (!trade.exited_ && !trade.isSimulated_) {
            ++openCount;
        }
    }

    if (openCount >= maxPosOpen_) {
        return;
    }

    if (!ranker_) {
        LG_ERROR("Strategy {} has no ranker", strategy_name_);
        return;
    }

    if (!universeSelector_) {
        LG_ERROR("Strategy {} has no universe selector", strategy_name_);
        return;
    }

    // ---------------------------------------------------------------------
    // 2. Select tradable universe
    // ---------------------------------------------------------------------
    CoinBarMap tradableBars = universeSelector_->select(
        bars,
        ts,
        indicators
    );

    if (tradableBars.empty()) {
        return;
    }

    // ---------------------------------------------------------------------
    // 3. Rank selected universe
    // ---------------------------------------------------------------------
    RankedUniverse ranked = ranker_->rank(
        tradableBars,
        ts,
        indicators
    );

    // ---------------------------------------------------------------------
    // 4. Attempt new entries
    // ---------------------------------------------------------------------
    unsigned int rankingPosition = 0;

    for (const auto& rankedCoin : ranked) {
        ++rankingPosition;

        if (openCount >= maxPosOpen_ ||
            rankingPosition > maxRankingPosition_) {
            break;
        }

        const Coin& coin = rankedCoin.coin;

        if (hasOpenTrade(current_trades, coin)) {
            continue;
        }

        if (!shouldEnter(
                coin,
                marketData,
                ts,
                current_trades,
                strategy_allocation,
                indicators
            )) {
            continue;
        }

        Trade trade = buildTrade(
            coin,
            marketData,
            ts,
            last_trade_id,
            strategy_allocation,
            live_trading,
            indicators
        );

        if (!trade.isSimulated_) {
            ++openCount;
        }

        current_trades.emplace_back(std::move(trade));
    }
}


/**************************************************************************************
 * Purpose : Check whether a coin already has an open trade
 **************************************************************************************/
bool Strategy::hasOpenTrade(
    const std::vector<Trade>& trades,
    const Coin& coin
) const
{
    return std::any_of(
        trades.begin(),
        trades.end(),
        [&](const Trade& trade) {
            return !trade.exited_ && trade.coin_ == coin;
        }
    );
}