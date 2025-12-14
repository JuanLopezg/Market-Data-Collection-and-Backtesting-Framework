#include "data_types.h"
#include "portfolio.h"
#include <vector>
#include "strategy.h"


class Backtester {
public:
    Backtester(const EnrichedData& marketData,Timestamp start,Timestamp end, Strategy strategy);

    void run();

private:
    const EnrichedData& marketData_;
    Portfolio portfolio_;
    std::vector<Trade> current_trades_;
    Strategy strategy_;

    Timestamp start_;
    Timestamp end_;

    void updatePortfolio(){
        portfolio_.updatePortfolio(current_trades_);
    }

    void calculateSignals(const CoinBarMap& bars ,Timestamp& ts){
        stratety_.calculateSignals(current_trades_, bars, ts);
    }
};
