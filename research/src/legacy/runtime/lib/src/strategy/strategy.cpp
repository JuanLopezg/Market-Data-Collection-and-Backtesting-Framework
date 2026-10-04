#include "strategy.h"

#include <algorithm>
#include <utility>

#include "indicator_engine.h"
#include "logger.h"
#include "no_ranker.h"
#include <sstream>
#include <vector>

namespace {

void closeTradeAtCurrentPrice(
    Trade& trade,
    Timestamp ts,
    double commissionExitFactor
)
{
    trade.exit_ = trade.current_price_;
    trade.end_ = ts;
    trade.exited_ = true;

    if (trade.direction_ == Direction::Long) {
        trade.pnl_ = (trade.exit_ - trade.entry_) * trade.size_;
    } else if (trade.direction_ == Direction::Short) {
        trade.pnl_ = (trade.entry_ - trade.exit_) * trade.size_;
    } else {
        trade.pnl_ = 0.0;
    }

    trade.commission_ += trade.exit_ * trade.size_ * commissionExitFactor;
}

} // namespace


Strategy::Strategy(
    std::string name,
    unsigned int maxPosOpen,
    double riskPerTradePctg,
    std::unique_ptr<UniverseSelector> universeSelector,
    std::unique_ptr<Ranker> ranker,
    double commissionEntryFactor,
    double commissionExitFactor,
    unsigned int maxRankingPosition,
    std::vector<std::unique_ptr<MarketFilter>> marketFilters
)
    : strategy_name_(std::move(name)),
      maxPosOpen_(maxPosOpen),
      riskPerTrade_(riskPerTradePctg),
      universeSelector_(std::move(universeSelector)),
      ranker_(std::move(ranker)),
      marketFilters_(std::move(marketFilters)),
      maxRankingPosition_(maxRankingPosition),
      commissionEntryFactor_(commissionEntryFactor),
      commissionExitFactor_(commissionExitFactor)
{
    if (!universeSelector_) {
        universeSelector_ = std::make_unique<AllUniverseSelector>();
    }

    if (!ranker_) {
        ranker_ = std::make_unique<NoRanker>();
    }
}


std::vector<IndicatorSpec> Strategy::requiredIndicators() const
{
    std::vector<IndicatorSpec> specs;

    for (const auto& filter : marketFilters_) {
        if (!filter) {
            continue;
        }

        const auto filterSpecs = filter->requiredIndicators();

        specs.insert(
            specs.end(),
            filterSpecs.begin(),
            filterSpecs.end()
        );
    }

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


bool Strategy::marketFiltersPass(
    const MarketData& marketData,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    for (const auto& filter : marketFilters_) {
        if (filter && !filter->passes(marketData, ts, indicators)) {
            return false;
        }
    }

    return true;
}


bool Strategy::usesEntryOrders() const
{
    return false;
}


Order Strategy::buildOrder(
    const Coin&,
    const MarketData&,
    Timestamp,
    double,
    bool,
    const IndicatorEngine&
) const
{
    LG_ERROR(
        "Strategy {} uses entry orders but buildOrder() was not implemented",
        strategy_name_
    );

    return Order{};
}


bool Strategy::shouldFillOrder(
    const Order&,
    const MarketData&,
    Timestamp,
    const IndicatorEngine&
) const
{
    return false;
}


Trade Strategy::buildTradeFromOrder(
    const Order&,
    const MarketData&,
    Timestamp,
    unsigned int&,
    bool,
    const IndicatorEngine&
) const
{
    LG_ERROR(
        "Strategy {} filled an order but buildTradeFromOrder() was not implemented",
        strategy_name_
    );

    Trade trade;
    trade.strategy_name_ = strategy_name_;
    trade.isSimulated_ = true;
    trade.exited_ = true;

    return trade;
}


bool Strategy::shouldCancelOrder(
    const Order&,
    const MarketData&,
    Timestamp,
    const IndicatorEngine&
) const
{
    return false;
}


/**********************************************************************************
 * Purpose : Execute strategy logic for a single timestamp
 **********************************************************************************/
void Strategy::calculateSignals(
    const MarketData& marketData,
    Timestamp ts,
    unsigned int& last_trade_id,
    std::vector<Trade>& current_trades,
    std::vector<Order>& pending_orders,
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
    // 1. Update open trades first.
    //
    // Important:
    // Market filters only block NEW entries.
    // Existing trades must still be updated/exited even when filters fail.
    // ---------------------------------------------------------------------
    unsigned int openCount = 0;

    for (auto& trade : current_trades) {
        if (trade.exited_) {
            LG_ERROR("Received a closed trade");
            continue;
        }

        const auto barIt = bars.find(trade.coin_);

        if (barIt == bars.end()) {
            LG_ERROR(
                "No data for coin {} on timestamp {}, closing trade at current_price={}",
                trade.coin_,
                ts,
                trade.current_price_
            );

            closeTradeAtCurrentPrice(
                trade,
                ts,
                commissionExitFactor_
            );

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

    // ---------------------------------------------------------------------
    // 2. Update pending entry orders.
    //
    // Orders are checked before new entries are created.
    // Therefore, an order created today cannot fill on the same bar.
    // It can only fill on a later bar.
    // ---------------------------------------------------------------------
    for (auto it = pending_orders.begin(); it != pending_orders.end(); ) {
        const Order& order = *it;

        const auto barIt = bars.find(order.coin_);

        if (barIt == bars.end()) {
            LG_ERROR(
                "No data for pending order coin {} on timestamp {}, deleting pending order",
                order.coin_,
                ts
            );

            it = pending_orders.erase(it);
            continue;
        }

        if (openCount >= maxPosOpen_) {
            if (shouldCancelOrder(order, marketData, ts, indicators)) {
                it = pending_orders.erase(it);
                continue;
            }

            ++it;
            continue;
        }

        if (shouldFillOrder(order, marketData, ts, indicators)) {
            Trade trade = buildTradeFromOrder(
                order,
                marketData,
                ts,
                last_trade_id,
                live_trading,
                indicators
            );

            if (!trade.exited_ && !trade.isSimulated_) {
                ++openCount;
            }

            current_trades.emplace_back(std::move(trade));
            it = pending_orders.erase(it);
            continue;
        }

        if (shouldCancelOrder(order, marketData, ts, indicators)) {
            it = pending_orders.erase(it);
            continue;
        }

        ++it;
    }

    unsigned int activeEntryCount =
        openCount + static_cast<unsigned int>(pending_orders.size());

    if (activeEntryCount >= maxPosOpen_) {
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
    // 3. Check strategy-level market filters.
    //
    // If these fail, the strategy opens no new trades and creates no new
    // pending orders on this timestamp.
    // ---------------------------------------------------------------------
    if (!marketFiltersPass(marketData, ts, indicators)) {
        return;
    }

    // ---------------------------------------------------------------------
    // 4. Select tradable universe
    // ---------------------------------------------------------------------
    CoinBarMap tradableBars = universeSelector_->select(
        bars,
        ts,
        indicators
    );

    if (tradableBars.empty()) {
        LG_INFO("No tradable bars at ts {}", ts);
        return;
    }

    // ---------------------------------------------------------------------
    // 5. Rank selected universe
    // ---------------------------------------------------------------------
    RankedUniverse ranked = ranker_->rank(
        tradableBars,
        ts,
        indicators
    );

    // ---------------------------------------------------------------------
    // 6. Attempt new entries
    // ---------------------------------------------------------------------
    unsigned int rankingPosition = 0;

    for (const auto& rankedCoin : ranked) {
        ++rankingPosition;

        if (activeEntryCount >= maxPosOpen_ ||
            rankingPosition > maxRankingPosition_) {
            break;
        }

        const Coin& coin = rankedCoin.coin;

        if (hasOpenTrade(current_trades, coin) ||
            hasPendingOrder(pending_orders, coin)) {
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

        if (usesEntryOrders()) {
            Order order = buildOrder(
                coin,
                marketData,
                ts,
                strategy_allocation,
                live_trading,
                indicators
            );

            pending_orders.emplace_back(std::move(order));
            ++activeEntryCount;
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

        if (!trade.exited_ && !trade.isSimulated_) {
            ++openCount;
            ++activeEntryCount;
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


/**************************************************************************************
 * Purpose : Check whether a coin already has a pending order
 **************************************************************************************/
bool Strategy::hasPendingOrder(
    const std::vector<Order>& orders,
    const Coin& coin
) const
{
    return std::any_of(
        orders.begin(),
        orders.end(),
        [&](const Order& order) {
            return order.coin_ == coin;
        }
    );
}