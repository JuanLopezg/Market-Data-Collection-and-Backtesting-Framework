#include "data_types.h"

#include <string>

/**************************************************************************************
 * Purpose : Save an equity/balance curve to a CSV file.
 *           Each element of `curve` contains a pair of (Balance, Equity) values
 *           representing the account state at a specific step in the backtest.
 *           The function writes the values sequentially so the file can be used
 *           later for analysis or plotting.
 *
 * Args    : curve - Vector containing ordered pairs of <Balance, Equity>
 *           path  - Destination file path where the CSV file will be written
 *
 * Return  : void
 **************************************************************************************/
void saveCurveToCSV(const std::vector<std::pair<Balance, Equity>>& curve, const std::string path);