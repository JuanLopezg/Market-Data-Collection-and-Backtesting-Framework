#include "data_types.h"


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
