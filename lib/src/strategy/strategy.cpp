#include "strategy.h"
#include "logger.h"

/**********************************************************************************
 * Purpose : Execute strategy logic for a single timestamp
 *
 * This method is FINAL and defines the invariant execution pipeline:
 *
 *  1. Update all currently open trades (PnL, exits, stop-loss, etc.)
 *  2. Count active (non-simulated) open positions
 *  3. Rank the trading universe using the configured Ranker
 *  4. Attempt new trade entries until limits are reached
 *
 * Concrete strategies customize behavior by overriding:
 *  - shouldEnter()
 *  - buildTrade()
 *  - onBar()
 *
 * Args :
 *   bars                - market data for all coins at the current timestamp
 *   ts                  - current timestamp
 *   last_trade_id       - reference to the global trade identifier counter
 *   current_trades      - reference to currently open trades (modifiable)
 *   strategy_allocation - capital allocated to this strategy
 *
 * Return : None
 **********************************************************************************/
void Strategy::calculateSignals(
    const CoinBarMap& bars,
    Timestamp ts,
    unsigned int& last_trade_id,
    std::vector<Trade>& current_trades,
    double strategy_allocation)
{
    // ---------------------------------------------------------------------
    // 1. Update open trades
    // ---------------------------------------------------------------------
    unsigned int openCount = 0;

    for (auto& trade : current_trades) {
        if (trade.exited_) {
            LG_ERROR("Received a closed trade");
            continue;
        }

        // Ensure market data exists for this coin
        auto it = bars.find(trade.coin_);
        if (it == bars.end()) {
            LG_ERROR("No data for coin {}", trade.coin_);
            continue;
        }

        // Delegate per-bar trade logic to the strategy
        onBar(trade, it->second, ts);

        // Count only real (non-simulated) open trades
        if (!trade.exited_ && !trade.isSimulated_) {
            ++openCount;
        }
    }

    // Stop early if position limit already reached
    if (openCount >= maxPosOpen_)
        return;

    // ---------------------------------------------------------------------
    // 2. Rank trading universe
    // ---------------------------------------------------------------------
    RankedBars ranked = ranker_->rank(bars);

    // ---------------------------------------------------------------------
    // 3. Attempt new entries
    // ---------------------------------------------------------------------
    unsigned int rankingPosition = 0;

    for (const auto& wrapped : ranked) {

        if (openCount >= maxPosOpen_ ||
            ++rankingPosition > maxRankingPosition_)
            break;

        const auto& [coin, bar] = wrapped.get();

        // Skip coins with an existing open trade
        if (hasOpenTrade(current_trades, coin))
            continue;

        // Strategy-specific entry condition
        if (!shouldEnter(coin, bar, current_trades))
            continue;

        // Build and register new trade
        Trade t = buildTrade(
            coin,
            bar,
            ts,
            last_trade_id,
            strategy_allocation
        );

        current_trades.emplace_back(std::move(t));
        ++openCount;
    }
}


/**************************************************************************************
 * Purpose : Check whether a coin already has an open (non-exited) trade
 *
 * Args :
 *   trades - collection of current open trades
 *   coin   - coin to check
 *
 * Return :
 *   true if a non-exited trade exists for the given coin, false otherwise
 **************************************************************************************/
bool Strategy::hasOpenTrade(const std::vector<Trade>& trades,
                    const Coin& coin) const
{
    return std::any_of(trades.begin(), trades.end(),
        [&](const Trade& t) {
            return !t.exited_ && t.coin_ == coin;
        });
}