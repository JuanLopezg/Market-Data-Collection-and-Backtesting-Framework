#include "backtester.h"
#include "database_utils.h"
#include "logger.h"
#include "research_strategy_definitions.h"
#include "sensitivity_study.h"
#include "xh_breakout_strategy.h"

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

// XHBreakout and XHBreakout_ATR with this study's robustness ranges. Sizing is the
// shared one-strategy equal-weight portfolio (research_studies::makeStudyPortfolio).
std::vector<StrategyDefinition> makeXhRobustnessDefinitions()
{
    std::vector<StrategyDefinition> definitions;

    definitions.push_back(StrategyDefinition{
        "XHBreakout",
        {
            {"xH", 30.0},
            {"fastMovingAverageLength", 5.0},
            {"momentumLength", 30.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 9999999.0}
        },
        makeSensitivityParameters({
            {"xH", "XH lookback", 10.0, 100.0, 5.0, true},
            {"fastMovingAverageLength", "Fast moving-average length", 4.0, 15.0, 1.0, true},
            {"momentumLength", "Momentum length", 20.0, 60.0, 4.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(
                parameters,
                "xH",
                30U
            );

            const unsigned int fastMovingAverageLength = unsignedParameterOr(
                parameters,
                "fastMovingAverageLength",
                5U
            );

            const unsigned int momentumLength = unsignedParameterOr(
                parameters,
                "momentumLength",
                30U
            );

            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters,
                "maxPositionsOpen",
                10U
            );

            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<XHBreakoutStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                xH,
                fastMovingAverageLength
            );
        },
        [](const ParameterValues&) {
            return true;
        }
    });


    definitions.push_back(StrategyDefinition{
        "XHBreakout_ATR",
        {
            {"xH", 30.0},
            {"atrLength", 14.0},
            {"atrMultiplier", 3.0},
            {"momentumLength", 30.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 9999999.0}
        },
        makeSensitivityParameters({
            {"xH", "XH lookback", 10.0, 100.0, 5.0, true},
            {"atrLength", "ATR length", 10.0, 30.0, 2.0, true},
            {"atrMultiplier", "ATR stop multiplier", 1.0, 10.0, 1.0, false},
            {"momentumLength", "Momentum length", 20.0, 60.0, 4.0, true},
            {"quantityPercent", "Quantity (%)", 2.0, 20.0, 2.0, false},
            {"maxPositionsOpen", "Maximum open positions", 1.0, 20.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(
                parameters,
                "xH",
                30U
            );

            const unsigned int atrLength = unsignedParameterOr(
                parameters,
                "atrLength",
                14U
            );

            const double atrMultiplier = parameterOr(
                parameters,
                "atrMultiplier",
                3.0
            );

            const unsigned int momentumLength = unsignedParameterOr(
                parameters,
                "momentumLength",
                30U
            );

            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters,
                "maxPositionsOpen",
                10U
            );

            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters,
                "maxRankingPosition",
                9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();

            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<XHBreakoutStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                xH,
                atrLength,
                atrMultiplier
            );
        },
        [](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(
                parameters,
                "xH",
                30U
            );

            const unsigned int atrLength = unsignedParameterOr(
                parameters,
                "atrLength",
                14U
            );

            const double atrMultiplier = parameterOr(
                parameters,
                "atrMultiplier",
                3.0
            );

            const unsigned int momentumLength = unsignedParameterOr(
                parameters,
                "momentumLength",
                30U
            );

            const double quantityPercent = parameterOr(
                parameters,
                "quantityPercent",
                10.0
            );

            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters,
                "maxPositionsOpen",
                10U
            );

            return xH > 0U &&
                   atrLength > 0U &&
                   std::isfinite(atrMultiplier) &&
                   atrMultiplier > 0.0 &&
                   momentumLength > 0U &&
                   std::isfinite(quantityPercent) &&
                   quantityPercent > 0.0 &&
                   maxPositionsOpen > 0U;
        }
    });


    return definitions;
}



} // namespace

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasePath;
    std::string databaseTimeframe;
    std::string benchmarkSymbol;
    std::string studyId;
    std::string outputRoot;
    std::string grid;
    std::string selectedStrategy;
    Timestamp startDate;
    Timestamp endDate;
    double feeTaker;
    options::options_description arguments("XH robustness study options");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Input CSV/SQLite")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD (inclusive)")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD (inclusive)")
        ("timeframe", options::value(&databaseTimeframe)->default_value("1d"), "Annualization timeframe")
        ("benchmark", options::value(&benchmarkSymbol)->default_value("BTC"), "Annualization benchmark")
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
            studyId = "current_xh_" + studyId;
        }
        if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\") != std::string::npos)
            throw std::invalid_argument("Study ID must be a single directory name");
    } catch (const std::exception& exception) {
        std::cerr << "Invalid study options: " << exception.what() << '\n';
        return 1;
    }

    Logger::Instance().Setup(true, false); // DEBUG on, stdout only

    const double periodsPerYear = periodsPerYearFromTimeframe(databaseTimeframe);
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

    std::vector<StrategyDefinition> strategyDefinitions = makeXhRobustnessDefinitions();
    if (selectedStrategy != "all") {
        std::erase_if(strategyDefinitions, [&](const auto& definition) {
            return definition.name != selectedStrategy;
        });
    }
    if (strategyDefinitions.empty())
        throw std::invalid_argument("Unknown XH strategy: " + selectedStrategy);
    // Compact controls reduce only the sweep. Full ranges and disabled axes
    // retain this executable's original robustness definitions.
    if (grid == "compact") {
        for (auto& definition : strategyDefinitions) {
            for (auto& parameter : definition.sensitivityParameters) {
                if (parameter.enabled) {
                    parameter.minimum = definition.currentParameters.at(parameter.key);
                    parameter.maximum = parameter.minimum + parameter.spacing;
                }
            }
        }
    }
    std::ofstream executionSettings(studyDirectory / "execution_settings.csv");
    executionSettings.exceptions(std::ios::failbit | std::ios::badbit);
    executionSettings << "runtime,start,end,grid,cutoff_policy\n"
                      << "current-lib-stop-entry," << startDate << ',' << endDate << ','
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
        .metricsSettings = metricsSettings
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
            true, // timeframe and benchmark are options here
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
    std::cerr << "XH study failed: " << exception.what() << '\n';
    return 1;
}
