#include "data_types.h"
#include <algorithm>

int directionToMultiplier(Direction& dir){
    if(dir == Direction::Long){
        return 1;
    }
    if(dir == Direction::Short){
        return -1;
    }
    
    return 0;
}

bool hasOpenTrade(const std::vector<Trade>& trades,const Coin& coin) {
    return std::any_of(trades.begin(), trades.end(),
        [&](const Trade& t) {
            return !t.exited_ && t.coin_ == coin;
        });
}
