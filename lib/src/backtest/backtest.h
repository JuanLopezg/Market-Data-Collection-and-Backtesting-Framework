#include "data_types.h"
#include <vector>

class Strategy;

enum class Direction {Long,Short,Flat};
using TradeID = unsigned int;

struct Trade{
    TradeID   trade_id_      = 0;
    Timestamp start_         = 0;
    Timestamp end_           = 0;
    double    commission_    = 0.0;
    Coin      coin_          = "";
    Direction direction_     = Direction::Flat;
    double    current_price_ = 0.0;
    double    entry_         = 0.0;
    double    exit_          = 0.0;
    double    size_          = 0.0;
    double    sl_            = 0.0;
    bool      isSimulated_   = false;
    bool      exited_        = false;
};

class Portfolio{
public:

    Portfolio(Timestamp start) : start_(start){};

    double GetCurrentEquity(){
        return current_equity_;
    }
    double GetCurrentBalance(){
        return current_balance_;
    }
    unsigned int GetNSimulated(){
        return nSimulated_;
    }
    void updatePortfolio(std::vector<Trade>& current_trades);

private:
    Timestamp start_ = 0;
    double current_equity_ = 100000.0;
    double current_balance_ = 100000.0;
    // balance, equity
    std::vector<std::pair<double,double>> balance_equity_historic_;
    std::map<TradeID,Trade> trades_history_; // closed real trades (non-simulated)
    unsigned int nSimulated_ = 0; // number of trades signaled and not taken
};

class Backtester {
public:
    Backtester(const EnrichedData& marketData,Timestamp start,Timestamp end);

    void run();

private:
    const EnrichedData& marketData_;
    Portfolio portfolio_;
    std::vector<Trade> current_trades_;
    Strategy strategy_;

    Timestamp start_;
    Timestamp end_;

    TradeID last_trade_id_ = 0;

    void updatePortfolio(){
        portfolio_.updatePortfolio(current_trades_);
    }

    void calculateSignals(const CoinBarMap& bars ,Timestamp& ts){
        stratety_.calculateSignals(current_trades_, bars, ts);
    }
};
