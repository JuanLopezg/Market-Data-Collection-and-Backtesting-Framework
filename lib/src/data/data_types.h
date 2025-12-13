#include <map>
#include <string>

/**************************************************************************************
 * Purpose : Represents a single OHLCV candle for one trading day.
 *
 * Members :
 *    open   - Opening price of the day
 *    high   - Highest price reached during the day
 *    low    - Lowest price reached during the day
 *    close  - Closing price of the day
 *    volume - Total traded volume for the day
 **************************************************************************************/
struct OHLCV {
    double open;
    double high;
    double low;
    double close;
    double volume;
};


/**************************************************************************************
 * Purpose : Container for OHLCV data grouped by trading pair and date.
 *
 * Structure :
 *    data[pair][date] = OHLCV
 *
 *    pair : A trading symbol such as "BTCUSDT".
 *    date : Integer formatted as YYYYMMDD for compact storage and fast lookup.
 *
 * Example :
 *    OHLCVData d;
 *    d.data["BTCUSDT"][20240118] = {open, high, low, close, volume};
 *
 * Use Cases :
 *    - Backtesting engines
 *    - Data preprocessing
 *    - Historical analytics (multi-pair)
 **************************************************************************************/
struct OHLCVData {
    std::map<std::string, std::map<unsigned int, OHLCV>> data;
};


struct BarData{
    // OHLCV
    double open;
    double high;
    double low;
    double close;
    double volume;

    unsigned int barNumber = 0;
};

using Timestamp =  int;
using Coin = std::string;
using CoinBarMap = std::map<Coin, BarData>;
using EnrichedData =  std::map<Timestamp, CoinBarMap>;