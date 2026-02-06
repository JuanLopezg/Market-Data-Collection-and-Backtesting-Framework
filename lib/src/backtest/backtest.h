#include "portfolio.h"
#include <vector>
#include <filesystem>
#include "strategy.h"
#include <sqlite3.h>


class Backtester {
public:
    Backtester(
        const EnrichedData& marketData,
        Timestamp start,
        Timestamp end,
        Strategy& strategy,
        std::filesystem::path database_path
    );

    void run();

private:
    const EnrichedData& marketData_;
    Portfolio portfolio_;
    std::vector<Trade> current_trades_;
    Strategy& strategy_;

    Timestamp start_;
    Timestamp end_;
    Timestamp starting_date_;

    std::filesystem::path database_path_;

    void updatePortfolio(){
        portfolio_.updatePortfolio(current_trades_);
    }

    void calculateSignals(const CoinBarMap& bars ,Timestamp& ts){
        strategy_.calculateSignals(current_trades_, bars, ts);
    }

    void storeTrades(sqlite3* db);

    void storeBalanceEquity(sqlite3* db);

    void storeResults();
};
