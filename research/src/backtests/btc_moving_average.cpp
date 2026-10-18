#include "backtester.h"
#include "database_utils.h"
#include "logger.h"
#include "research_strategy_definitions.h"
#include "sensitivity_study.h"

#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace research_studies;

// MRShort and PureMom with only their BTC moving-average length swept.
std::vector<StrategyDefinition> makeBtcMovingAverageDefinitions()
{
    auto definitions = makeStrategyDefinitions();
    std::erase_if(definitions, [](const auto& definition) {
        return definition.name != "MRShort" && definition.name != "PureMom";
    });
    // Reuse the current signal and portfolio configurations, while preserving
    // this study's original fixed parameters and enabled BTC SMA sweep.
    for (auto& definition : definitions) {
        if (definition.name == "MRShort") {
            definition.sensitivityParameters = {
                {"rsiLength", "RSI length", 5.0, 5.0, 1.0, false},
                {"rsiEntry", "RSI entry", 70.0, 70.0, 1.0, false},
                {"heldBars", "Held bars", 3.0, 3.0, 1.0, false},
                {"entryAtrMultiple", "Entry ATR multiple", 0.30, 0.30, 0.10, false},
                {"entryAtrLength", "Entry ATR length", 5.0, 5.0, 1.0, false},
                {"btcMovingAverageLength", "BTC moving-average length", 10.0, 200.0, 5.0, true},
                {"rankerRocLength", "Ranker ROC length", 30.0, 30.0, 1.0, false},
                {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
                {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
            };
        }
        if (definition.name == "PureMom") {
            definition.sensitivityParameters = {
                {"heldBars", "Held bars", 7.0, 7.0, 1.0, false},
                {"rocLength", "ROC length", 7.0, 7.0, 1.0, false},
                {"btcMovingAverageLength", "BTC moving-average length", 10.0, 200.0, 5.0, true},
                {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
                {"maxPositionsOpen", "Maximum open positions", 3.0, 3.0, 1.0, false}
            };
        }
    }
    return definitions;
}

} // namespace

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasePath;
    const std::string databaseTimeframe = "1d";
    const std::string benchmarkSymbol = "BTC";
    std::string studyId;
    std::string outputRoot;
    std::string grid;
    std::string selectedStrategy;
    Timestamp startDate;
    Timestamp endDate;
    double feeTaker;
    options::options_description arguments("BTC moving-average study options");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Input CSV/SQLite")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD (inclusive)")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD (inclusive)")
        ("study-id", options::value(&studyId)->default_value(""), "New study directory name (default UTC timestamp)")
        ("output-root", options::value(&outputRoot)->default_value("storage/backtests/sensitivity_results"), "Study parent directory")
        ("strategy", options::value(&selectedStrategy)->default_value("all"), "Strategy name or all")
        ("grid", options::value(&grid)->default_value("none"), "none, compact or full sensitivity grid")
        ("fee-taker", options::value(&feeTaker)->default_value(0.0), "Market commission fraction per fill");
    try {
        options::variables_map values;
        options::store(options::parse_command_line(argc, argv, arguments), values);
        if (values.count("help")) {
            std::cout << arguments << '\n';
            return 0;
        }
        options::notify(values);
        auto validDate = [](Timestamp date) {
            const std::chrono::year_month_day calendarDate{
                std::chrono::year(static_cast<int>(date / 10000)),
                std::chrono::month((date / 100) % 100),
                std::chrono::day(date % 100)
            };
            return date >= 19000101 && date <= 99991231 && calendarDate.ok();
        };
        if (!validDate(startDate) || !validDate(endDate) || startDate > endDate)
            throw std::invalid_argument("Require valid start <= end dates in YYYYMMDD format");
        if (grid != "none" && grid != "compact" && grid != "full")
            throw std::invalid_argument("Grid must be none, compact or full");
        if (!std::isfinite(feeTaker) || feeTaker < 0.0)
            throw std::invalid_argument("Fee must be finite and non-negative");
        if (studyId.empty()) {
            studyId = utcTimestamp();
            studyId.erase(std::remove(studyId.begin(), studyId.end(), ':'), studyId.end());
            studyId = "current_btc_ma_" + studyId;
        }
        if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\") != std::string::npos)
            throw std::invalid_argument("Study ID must be a single directory name");
    } catch (const std::exception& exception) {
        LG_ERROR("Invalid study options: {}", exception.what());
        return 1;
    }

    Logger::Instance().Setup(true, false); // DEBUG on, stdout only

    const double periodsPerYear = 365.0;
    constexpr double initialBalance = 100000.0;
    constexpr double feeMaker = 0.0;
    // Kept as zero metadata columns: commissions come from actual taker fills.
    constexpr double commissionEntryFactor = 0.0;
    constexpr double commissionExitFactor = 0.0;
    const bool runSensitivityGrid = grid != "none";

    const BacktestMetricsSettings metricsSettings{
        .periodsPerYear = periodsPerYear,
        .excludeSimulatedTrades = true,
        .databaseTimeframe = databaseTimeframe,
        .annualizationBenchmarkSymbol = benchmarkSymbol
    };

    const std::filesystem::path sensitivityRootDirectory = outputRoot;

    const std::filesystem::path studyDirectory =
        sensitivityRootDirectory / studyId;

    const std::filesystem::path reportsDirectory =
        studyDirectory / "reports";

    if (std::filesystem::exists(studyDirectory))
        throw std::invalid_argument("Study directory already exists; choose a new study ID");

    if (!ensureDirectoryExists(studyDirectory) ||
        !ensureDirectoryExists(reportsDirectory)) {
        return 1;
    }

    LG_INFO("Database loading started");
    if (!std::filesystem::is_regular_file(databasePath))
        throw std::invalid_argument("Input database does not exist: " + databasePath);
    OHLCVData ohlcvData = loadDatabase(databasePath, startDate, endDate);
    if (ohlcvData.data.empty() || !ohlcvData.data.contains(benchmarkSymbol))
        throw std::invalid_argument("Selected window must contain market data and the annualization benchmark");
    LG_INFO("Database loaded successfully");
    LG_INFO("Annualization: {} bars, benchmark {}, {} periods per year", databaseTimeframe, benchmarkSymbol, periodsPerYear);

    std::vector<StrategyDefinition> strategyDefinitions = makeBtcMovingAverageDefinitions();
    if (selectedStrategy != "all") {
        std::erase_if(strategyDefinitions, [&](const auto& definition) {
            return definition.name != selectedStrategy;
        });
    }
    if (strategyDefinitions.empty())
        throw std::invalid_argument("Unknown BTC study strategy: " + selectedStrategy);
    std::ofstream executionSettings(studyDirectory / "execution_settings.csv");
    executionSettings.exceptions(std::ios::failbit | std::ios::badbit);
    executionSettings << "runtime,start,end,grid,cutoff_policy\n"
                      << "current-lib," << startDate << ',' << endDate << ','
                      << grid << ",mark-open-positions\n";

    try {
        writeParameterGridCsv(
            studyDirectory / "parameter_grid.csv",
            studyId,
            strategyDefinitions
        );
    } catch (const std::exception& exception) {
        LG_ERROR("{}", exception.what());
        return 1;
    }

    const StudyRunSettings runSettings{
        .makePortfolio = makeStudyPortfolio,
        .initialBalance = initialBalance,
        .feeTaker = feeTaker,
        .metricsSettings = metricsSettings,
        .keepEndpointsOnly = grid == "compact"
    };

    StudyResults results;
    try {
        results = runStudyStrategies(
            strategyDefinitions,
            ohlcvData,
            runSettings,
            runSensitivityGrid,
            studyDirectory,
            reportsDirectory,
            studyId
        );

        writeStudyMetadataCsv(
            studyDirectory / "study_metadata.csv",
            studyId,
            databasePath,
            initialBalance,
            feeMaker,
            feeTaker,
            commissionEntryFactor,
            commissionExitFactor,
            metricsSettings,
            false, // fixed 1d/BTC annualization: original columns only
            results.sensitivityStats
        );
    } catch (const std::exception& exception) {
        LG_ERROR("Backtest study stopped: {}", exception.what());
        return 1;
    }

    logStudySummary(results, studyDirectory);

    return 0;
}
catch (const std::exception& exception)
{
    LG_ALERT("BTC moving-average study failed: {}", exception.what());
    return 1;
}
