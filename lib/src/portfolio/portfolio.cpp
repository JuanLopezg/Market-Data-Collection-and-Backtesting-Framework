#include "portfolio.h"


int directionToMultiplier(Direction& dir){
    if(dir == 0){
        return 1;
    }
    if(dir == 1){
        return -1;
    }
    
    return 0
}

void Portfolio::updatePortfolio(std::vector<Trade>& current_trades){
    double floatingPNL = 0;
    double balance = this->current_balance_;

    for (auto it = current_trades.begin(); it != current_trades.end(); ) {
        Trade& trade = *it;

        if (trade.exited_) { // trades just closed
            if(!trade.isSimulated_){
                trades_history_[trade.trade_id_] = trade;
                balance += (trade.size_*(trade.exit_-trade.entry_)*directionToMultiplier(trade.direction_) - trade.commission_);
            }else{
                this->nSimulated_ ++;
            }
            it = current_trades.erase(it); 
        } 
        else { // ongoing trades
            if(!trade.isSimulated_){
                floatingPNL += (trade.size_*(trade.current_price_-trade.entry_)*directionToMultiplier(trade.direction_) - trade.commission_);
            }
            ++it;
        }
    }

    this->current_balance_ = balance;
    this->current_equity_ = balance + floatingPNL;
    this->balance_equity_historic_.emplace_back(std::make_pair(current_balance_,current_equity_));
}
