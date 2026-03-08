#include "csv_utils.h"
#include "logger.h"
#include <vector>
#include <fstream> 

// Custom locale to use comma as decimal separator
struct CommaDecimal : std::numpunct<char>{
    protected:
        char do_decimal_point() const override { return ','; }
};


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
void saveCurveToCSV(const std::vector<std::pair<Balance, Equity>>& curve, const std::string path)
{

    std::ofstream file(path);

    if (!file.is_open())
    {
        LG_ERROR(" Failed to open file: {}", path);
        return;
    }

    // Apply locale with comma decimal separator
    file.imbue(std::locale(file.getloc(), new CommaDecimal));

    file << "Index;Balance;Equity\n";

    for (size_t i = 0; i < curve.size(); ++i)
    {
        file << i << ";"
             << curve[i].first << ";"
             << curve[i].second << "\n";
    }

    file.close();

    LG_INFO("Curve saved to: {}", path);
}
