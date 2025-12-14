#include "data_types"

bool hasOpenTrade(const std::vector<Trade>& trades,const Coin& coin) {
    return std::any_of(trades.begin(), trades.end(),
        [&](const Trade& t) {
            return !t.exited_ && t.coin_ == coin;
        });
}