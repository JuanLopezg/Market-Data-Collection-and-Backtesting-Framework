#include "strategy.h"
#include <algorithm>
#include "time_utils.h"
#include "logger.h"


class StrategyHighBreakout : public Strategy {
public:
    StrategyHighBreakout(Portfolio& portfolio, double commissionEntryPctg, double commissionExitPctg): Strategy(portfolio, 10, Ranking::Volume, commissionEntryPctg,  commissionExitPctg) {} 

    inline unsigned int processSignal(std::vector<Trade>& current_trades, Coin& coin, const BarData& bar, Timestamp& ts){
        if(bar.close > bar.high_20d && bar.barNumber > 20){
            Trade newTrade;
            newTrade.trade_id_ = last_trade_id_ ++ ;
            newTrade.start_ = nextDay(ts);
            newTrade.commission_ += this->commissionEntryPctg_;
            newTrade.coin_ = coin;
            newTrade.direction_ = Direction::Long;
            newTrade.current_price_ = bar.close;
            newTrade.entry_ = bar.close;
            newTrade.size_ = 0.05 * this->portfolio_.GetCurrentBalance() / bar.close;
            newTrade.sl_ = bar.close - 3*bar.atr_14d;
            newTrade.slReference_ = bar.close;

            current_trades.emplace_back(newTrade);
            return 1;
        }

        return 0;
    }


    inline unsigned int processOpenTrades(std::vector<Trade>& current_trades, const CoinBarMap& bars, Timestamp ts){
        unsigned int openCount = 0;

        for (auto& trade : current_trades) {

            if (trade.exited_) {
                LG_ERROR("Received a closed trade");
                continue;
            }

            // Try to find market data for this trade's coin
            auto it = bars.find(trade.coin_);
            if (it == bars.end()) {
                LG_ERROR("No data for coin {}", trade.coin_);
                continue; // no data for this coin at this timestamp
            }

            const BarData& bar = it->second;

            // Update current price (example: close price)
            trade.current_price_ = bar.close;
            trade.end_ = nextDay(ts); // data is given at closing of each day
            trade.pnl_ = (trade.size_*(trade.current_price_-trade.entry_)*directionToMultiplier(trade.direction_));

            if (trade.direction_ == Direction::Long) {
                if(ts == trade.start_){
                    if(bar.low < trade.entry_){
                        trade.isSimulated_ = false;
                    }
                }
                if (bar.low <= trade.sl_) {
                    trade.exit_   = trade.sl_;
                    trade.exited_ = true;
                    trade.commission_ += this->commissionExitPctg_;
                    continue;
                }else{
                    if(trade.slReference_ < bar.high){
                        trade.slReference_ = bar.high;
                        trade.sl_ = trade.slReference_ - 3*bar.atr_14d;
                    }
                }
            }

            // Trade still open
            if(!trade.isSimulated_){
                ++openCount;
            }
        }

        return openCount;

    };

    inline void calculateSignals(std::vector<Trade>& current_trades, const CoinBarMap& bars, Timestamp ts) override {
        unsigned int nOpenTrades = processOpenTrades(current_trades, bars, ts);

        if(nOpenTrades < this->maxPosOpen_){

            RankedBars rbars = rank(bars, this->ranking_);

            unsigned int counter = 0;
            unsigned int universeVolume = 20;

            for (const auto& wrapped : rbars) {
                counter++;

                if (counter > universeVolume || nOpenTrades >= this->maxPosOpen_) 
                    break;
                
                const auto& [coin, bar] = wrapped.get();

                if(hasOpenTrade(current_trades, coin))
                    continue;

                // ---- ENTRY LOGIC ----
                nOpenTrades += processSignal(current_trades, coin, bar, ts);
            }


        }

        
        
    }
};
