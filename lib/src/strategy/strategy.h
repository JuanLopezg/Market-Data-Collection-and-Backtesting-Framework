#pragma once

#include <vector>
#include "data_types.h"  
#include "portafolio.h"

enum class Ranking{Volume, Return, None};
using RankedBars = std::vector<std::reference_wrapper<const std::pair<const Coin, BarData>>>;


TradeID last_trade_id_ = 0;


class Strategy {
public:
    virtual ~Strategy() = default;

    /**********************************************************************************
     * Purpose : Calculate trading signals for the current timestamp.
     * Args    :
     *   - current_trades : Reference to currently open trades (can be modified)
     *   - bars           : Market data for all coins at this timestamp
     *   - ts             : Current timestamp
     **********************************************************************************/
    virtual void calculateSignals(
        std::vector<Trade>& current_trades,
        const CoinBarMap& bars,
        Timestamp ts
    );

    inline RankedBars rank(const CoinBarMap& bars, Ranking ranking) {
        RankedBars ranked;
        ranked.reserve(bars.size());

        for (const auto& kv : bars) {
            ranked.emplace_back(kv);
        }

        if (ranking == Ranking::Volume) {
            std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) {
                    return a.get().second.volume > b.get().second.volume;
                }
            );
        }

        return ranked;
    }


protected:
    Strategy(Portafolio& portafolio, unsigned int maxPosOpen, Ranking ranking, double commissionEntryPctg, double commissionExitPctg): maxPosOpen_(maxPosOpen), ranking_(ranking),
            commissionEntryPctg_(commissionEntryPctg), commissionExitPctg_(commissionExitPctg), portafolio_(portafolio)   {}

    unsigned int maxPosOpen_;
    Ranking ranking_;
    double commissionEntryPctg_;
    double commissionExitPctg_;
    Portafolio& portafolio_
};
