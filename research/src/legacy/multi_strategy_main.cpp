#include "backtester.h"
#include "backtest_helpers.h"
#include "backtest_metrics.h"
#include "scenario_reports.h"
#include "research_strategy_definitions.h"
#include "donchian_signal_strategy.h"
#include "database_utils.h"
#include "logger.h"
#include <boost/program_options.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace {

using namespace scenario_reports;
using research_studies::makeTopLiquidityUniverse;

struct DatasetDefinition {
    std::string id;
    std::string fileName;
    std::string benchmarkSymbol;
    unsigned int benchmarkMovingAverageLength;
    double periodsPerYear = 365.0;
};

struct StrategyDefinition {
    std::string name;
    std::string outputSlug;
    std::function<std::unique_ptr<Strategy>(
        const std::string& benchmarkSymbol,
        unsigned int benchmarkMovingAverageLength
    )> create;
};

template<class Definition, class Name>
void selectDefinitions(std::vector<Definition>& definitions, const std::string& selected, Name name)
{
    if (selected == "all")
        return;
    std::set<std::string> names;
    std::istringstream input(selected);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (token.empty() || !names.insert(token).second)
            throw std::invalid_argument("Selection contains an empty or duplicate name");
    }
    if (names.empty())
        throw std::invalid_argument("Selection must not be empty");
    for (const auto& requested : names) {
        if (std::none_of(definitions.begin(), definitions.end(), [&](const auto& d) { return name(d) == requested; }))
            throw std::invalid_argument("Unknown study selection: " + requested);
    }
    std::erase_if(definitions, [&](const auto& d) { return !names.contains(name(d)); });
}

std::vector<StrategyDefinition> makeStrategyDefinitions()
{
    std::vector<StrategyDefinition> strategyDefinitions;

    // ------------------------------------------------------------------
    // BargainChaser
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "BargainChaser",
        "bargain_chaser",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 999999;
            constexpr unsigned int barsUntilExit = 1;
            constexpr double fallPercentage = 10.0;
            constexpr unsigned int movingAverageLength = 50;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    1
                },
                false
            );

            return std::make_unique<BargainChaserStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                barsUntilExit,
                fallPercentage,
                movingAverageLength
            );
        }
    });

    // ------------------------------------------------------------------
    // ATRBreakout
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "ATRBreakout",
        "atr_breakout",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int heldBars = 3;
            constexpr double atrMultiple = 0.875;
            constexpr unsigned int atrLength = 10;
            constexpr unsigned int momentumScoreNum = 30;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumScoreNum
                },
                true
            );

            return std::make_unique<ATRBreakoutStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                heldBars,
                atrMultiple,
                atrLength
            );
        }
    });

    // ------------------------------------------------------------------
    // MRShort
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "MRShort",
        "mr_short",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int rsiLength = 5;
            constexpr double rsiEntry = 65.0;
            constexpr double entryAtrMultiple = 0.30;
            constexpr unsigned int entryAtrLength = 10;
            constexpr unsigned int heldBars = 7;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    30
                },
                true
            );

            return std::make_unique<ShortMeanReversionStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                benchmarkMovingAverageLength,
                entryAtrMultiple,
                entryAtrLength,
                heldBars,
                benchmarkSymbol
            );
        }
    });

    // ------------------------------------------------------------------
    // PureMom
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "PureMom",
        "pure_mom",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int heldBars = 7;
            constexpr unsigned int rocLength = 7;
            constexpr unsigned int maxPositionsOpen = 3;
            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    rocLength
                },
                true
            );

            return std::make_unique<PureMomentumStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                heldBars,
                benchmarkMovingAverageLength,
                benchmarkSymbol
            );
        }
    });

    // ------------------------------------------------------------------
    // PureRSI
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "PureRSI",
        "pure_rsi",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int rsiLength = 7;
            constexpr double rsiEntry = 80.0;
            constexpr double rsiExit = 70.0;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 1000000;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::RSI,
                    PriceField::Close,
                    rsiLength
                },
                true
            );

            return std::make_unique<StrategyPureRSI>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                rsiExit
            );
        }
    });

    // ------------------------------------------------------------------
    // MRRSILong
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "MRRSILong",
        "mr_rsi_long",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int rsiLength = 3;
            constexpr double rsiEntryLevel = 10.0;
            constexpr unsigned int momentumLength = 30;
            constexpr unsigned int heldBars = 1;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition =
                std::numeric_limits<unsigned int>::max();

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<RSIMeanReversionStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                rsiLength,
                rsiEntryLevel,
                heldBars
            );
        }
    });

    // ------------------------------------------------------------------
    // XHBreakout
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "XHBreakout",
        "xh_breakout",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            (void)benchmarkSymbol;
            (void)benchmarkMovingAverageLength;

            constexpr unsigned int xH = 30;
            constexpr unsigned int fastMovingAverageLength = 5;
            constexpr unsigned int momentumLength = 30;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 9999999;

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
        }
    });


    // ------------------------------------------------------------------
    // DonchianBreakout
    // ------------------------------------------------------------------
    strategyDefinitions.push_back(StrategyDefinition{
        "DonchianBreakout",
        "donchian_breakout",
        [](
            const std::string& benchmarkSymbol,
            unsigned int benchmarkMovingAverageLength
        ) -> std::unique_ptr<Strategy> {
            constexpr unsigned int donchianLookback = 30;
            constexpr bool useMarketStateFilter = false;
            constexpr unsigned int momentumLength = 30;
            constexpr unsigned int maxPositionsOpen = 10;
            constexpr unsigned int maxRankingPosition = 9999999;

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = std::make_unique<IndicatorRanker>(
                IndicatorSpec{
                    IndicatorKind::ROC,
                    PriceField::Close,
                    momentumLength
                },
                true
            );

            return std::make_unique<DonchianSignalStrategy>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                donchianLookback,
                useMarketStateFilter,
                benchmarkMovingAverageLength,
                benchmarkSymbol
            );
        }
    });

    return strategyDefinitions;
}

} // namespace

int main(int argc, char** argv)
try
{
    namespace options = boost::program_options;
    std::string databasesRoot, outputRoot, studyId;
    std::string selectedStrategies, selectedDatasets;
    Timestamp startDate, endDate;
    double feeTaker;
    options::options_description arguments("Isolated scenario study options");
    arguments.add_options()
        ("help,h", "Show study options")
        ("databases-dir", options::value(&databasesRoot)->default_value("storage/databases"), "Scenario CSV directory")
        ("output-root", options::value(&outputRoot)->default_value("storage/backtests/multi_strategy"), "Study parent directory")
        ("study-id", options::value(&studyId)->default_value(""), "New directory name (default UTC timestamp)")
        ("strategy", options::value(&selectedStrategies)->default_value("PureRSI,XHBreakout,DonchianBreakout"), "Comma-separated names, or all")
        ("dataset", options::value(&selectedDatasets)->default_value("1d_cmc"), "Comma-separated dataset IDs, or all")
        ("start", options::value(&startDate)->default_value(20200101), "Inclusive dataset key YYYYMMDD")
        ("end", options::value(&endDate)->default_value(20200416), "Inclusive cutoff key YYYYMMDD")
        ("fee-taker", options::value(&feeTaker)->default_value(0.0), "Commission fraction per actual fill");
    options::variables_map values;
    options::store(options::parse_command_line(argc, argv, arguments), values);
    if (values.count("help")) {
        std::cout << arguments << '\n';
        return 0;
    }
    options::notify(values);
    auto validDate = [](Timestamp date) {
        return date >= 19000101 && date <= 99991231 && std::chrono::year_month_day{
            std::chrono::year(static_cast<int>(date / 10000)), std::chrono::month((date / 100) % 100),
            std::chrono::day(date % 100)}.ok();
    };
    if (!validDate(startDate) || !validDate(endDate) || startDate > endDate)
        throw std::invalid_argument("Require valid start <= end keys in YYYYMMDD format");
    if (!std::isfinite(feeTaker) || feeTaker < 0.0)
        throw std::invalid_argument("Fee must be finite and non-negative");
    if (studyId.empty()) {
        const auto now = std::time(nullptr);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        std::ostringstream name;
        name << "current_scenarios_" << std::put_time(&utc, "%Y%m%d_%H%M%S");
        studyId = name.str();
    }
    if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\") != std::string::npos)
        throw std::invalid_argument("Study ID must be a single directory name");

    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    const std::filesystem::path databasesDir = databasesRoot;
    const std::filesystem::path studyDir = std::filesystem::path(outputRoot) / studyId;
    std::vector<DatasetDefinition> datasets{
        // Preserve the original regime SMA bar counts for each dataset.
        // Intraday CSVs may encode successive bars as synthetic YYYYMMDD keys.
        {"1d_cmc", "1d_cmc.csv", "BTC", 50},
        {"stocks_daily", "stocks_dailyB.csv", "^GSPC", 50, 252.0},
        {"4h_binance", "4h_binance.csv", "BTCUSDT", 300, 365.0 * 6},
        {"8h_binance", "8h_binance.csv", "BTCUSDT", 150, 365.0 * 3},
        {"12h_binance", "12h_binance.csv", "BTCUSDT", 100, 365.0 * 2},
        {"1d_binance", "1d_binance.csv", "BTCUSDT", 50},
        {"1d_4h_shift_binance", "1d_4h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_8h_shift_binance", "1d_8h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_12h_shift_binance", "1d_12h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_16h_shift_binance", "1d_16h_shift_binance.csv", "BTCUSDT", 50},
        {"1d_20h_shift_binance", "1d_20h_shift_binance.csv", "BTCUSDT", 50},
        {"2d_binance", "2d_binance.csv", "BTCUSDT", 25, 365.0 / 2},
        {"3d_binance", "3d_binance.csv", "BTCUSDT", 17, 365.0 / 3},
        {"5d_binance", "5d_binance.csv", "BTCUSDT", 10, 365.0 / 5},
        {"7d_binance", "7d_binance.csv", "BTCUSDT", 7, 365.0 / 7},
        {"14d_binance", "14d_binance.csv", "BTCUSDT", 4, 365.0 / 14},
        {"30d_binance", "30d_binance.csv", "BTCUSDT", 2, 365.0 / 30},
        {"binance_oos", "binance_oos.csv", "BTCUSDT", 50},
    };

    auto strategyDefinitions = makeStrategyDefinitions();
    selectDefinitions(strategyDefinitions, selectedStrategies, [](const auto& d) { return d.name; });
    selectDefinitions(datasets, selectedDatasets, [](const auto& d) { return d.id; });
    if (std::filesystem::exists(studyDir))
        throw std::invalid_argument("Study directory already exists; choose a new study ID");
    if (!ensureDirectoryExists(studyDir))
        return 1;
    constexpr double initialBalance = 100000.0;
    nlohmann::json manifest{{"runtime", "current-lib"}, {"start", startDate}, {"end", endDate},
        {"initial_balance", initialBalance}, {"fee_taker", feeTaker}, {"cutoff_policy", "mark-open-positions"},
        {"runs", nlohmann::json::array()}};
    if (strategyDefinitions.empty()) {
        LG_ERROR("Enable at least one strategy.");
        return 1;
    }

    LG_INFO(
        "{} enabled strategies x {} datasets = {} isolated backtests planned",
        strategyDefinitions.size(),
        datasets.size(),
        strategyDefinitions.size() * datasets.size()
    );

    std::size_t successfulRuns = 0U;
    std::size_t failedRuns = 0U;

    // Only the enabled strategies that successfully complete the 1d_cmc
    // dataset are included in the cross-strategy equity/correlation report.
    std::vector<CorrelationChartEntry> oneDayCmcStrategyCharts;
    oneDayCmcStrategyCharts.reserve(strategyDefinitions.size());

    std::vector<BenchmarkPoint> oneDayCmcBtcPoints;
    std::string oneDayCmcBtcSymbol;

    for (const StrategyDefinition& strategyDefinition : strategyDefinitions) {
        std::vector<ChartEntry> chartEntries;
        chartEntries.reserve(datasets.size());

        for (const DatasetDefinition& dataset : datasets) {
            nlohmann::json run{{"strategy", strategyDefinition.name}, {"dataset", dataset.id},
                {"dataset_file", dataset.fileName},
                {"benchmark", dataset.benchmarkSymbol}, {"benchmark_sma_bars", dataset.benchmarkMovingAverageLength},
                {"periods_per_year", dataset.periodsPerYear}};
            const std::filesystem::path databasePath =
                databasesDir / dataset.fileName;

            if (!std::filesystem::exists(databasePath)) {
                LG_ERROR(
                    "Skipping {} / {} because the dataset does not exist: {}",
                    strategyDefinition.name,
                    dataset.id,
                    databasePath.string()
                );
                ++failedRuns;
                run["status"] = "skipped";
                run["failure_reason"] = "Dataset does not exist";
                manifest["runs"].push_back(run);
                continue;
            }

            const std::filesystem::path temporaryChartPath =
                studyDir /
                (
                    ".temporary_" + strategyDefinition.outputSlug + "_" +
                    dataset.id + "_balance_equity.html"
                );

            LG_INFO("============================================================");
            LG_INFO(
                "Running strategy={} dataset={} benchmark={} benchmarkSmaBars={}",
                strategyDefinition.name,
                dataset.fileName,
                dataset.benchmarkSymbol,
                dataset.benchmarkMovingAverageLength
            );

            try {
                // Load a fresh copy for every strategy/dataset pair to prevent
                // any state or indicator cache from leaking between runs.
                LG_INFO("Database loading started: {}", databasePath.string());
                OHLCVData ohlcvData = loadDatabase(
                    databasePath.string(),
                    startDate, endDate
                );
                LG_INFO("Database loaded successfully");

                if (ohlcvData.data.empty() || !ohlcvData.data.contains(dataset.benchmarkSymbol))
                    throw std::invalid_argument("Selected input must contain market data and its exact benchmark");
                StrategyPortfolio portfolio;
                portfolio.emplace_back(1, strategyDefinition.create(dataset.benchmarkSymbol,
                    dataset.benchmarkMovingAverageLength), 1.0,
                    std::make_unique<EqualWeightSizer>(0.10), RiskConstraints(1.5, 1.5),
                    std::make_unique<EntryExitOnlyRebalancePolicy>());
                // Each pair owns fresh strategy, indicators, account and actual campaigns.
                BacktestContext context(ohlcvData, std::move(portfolio), initialBalance, feeTaker);

                const MarketData& marketData = context.GetMarketData();
                LG_INFO("Market data and indicators initialized");

                Backtester tester(context);

                LG_INFO("Starting loop");
                tester.loop();
                LG_INFO("Finished loop");

                const std::string exportName = strategyDefinition.outputSlug + "_" + dataset.id;
                tester.storeTradesCSV(studyDir / (exportName + "_trades.csv"));
                std::ofstream accounts(studyDir / (exportName + "_account.csv"));
                accounts.exceptions(std::ios::failbit | std::ios::badbit);
                accounts << "timestamp,cash,balance,equity\n" << std::setprecision(17);
                for (const auto& point : context.GetAccountHistory())
                    accounts << point.timestamp << ',' << point.cash << ',' << point.balance << ',' << point.equity << '\n';
                BacktestMetricsSettings metricsSettings;
                metricsSettings.periodsPerYear = dataset.periodsPerYear;
                metricsSettings.annualizationBenchmarkSymbol = dataset.benchmarkSymbol;
                metricsSettings.monteCarloSimulationCount = 0;
                const auto metrics = calculateBacktestMetrics(strategyDefinition.name, context.GetTradesHistory(),
                    context.GetBalanceEquityHistoric(), marketData, initialBalance, metricsSettings);
                run["bars"] = marketData.size();
                run["closed_campaigns"] = metrics.tradeCount;
                run["final_equity"] = metrics.finalEquity;
                run["net_return_percent"] = metrics.netReturnPercent;
                run["annualized_return_percent"] = metrics.annualizedReturnPercent;

                printBalanceEquityChart(
                    context.GetBalanceEquityHistoric(),
                    marketData, {}, 60, dataset.periodsPerYear,
                    temporaryChartPath.string()
                );

                std::string chartHtml = readTextFile(temporaryChartPath);

                const std::vector<BenchmarkPoint> benchmarkPoints =
                    makeBenchmarkPoints(
                        marketData,
                        dataset.benchmarkSymbol
                    );

                const double benchmarkFirstClose = benchmarkPoints.front().close;
                const double benchmarkLastEquity =
                    initialBalance * benchmarkPoints.back().close /
                    benchmarkFirstClose;

                LG_INFO(
                    "Buy-and-hold curve: symbol={} points={} first={} "
                    "firstClose={} last={} lastEquity={}",
                    dataset.benchmarkSymbol,
                    benchmarkPoints.size(),
                    benchmarkPoints.front().timestamp,
                    benchmarkFirstClose,
                    benchmarkPoints.back().timestamp,
                    benchmarkLastEquity
                );

                PlotlyXYExpressions oneDayCmcEquityCurve;

                if (dataset.id == "1d_cmc") {
                    oneDayCmcEquityCurve =
                        extractNamedPlotlyXYExpressions(
                            chartHtml,
                            "Equity"
                        );

                    LG_INFO(
                        "Saved resolved 1d_cmc Equity arrays for {} "
                        "(dates={} bytes, equity={} bytes)",
                        strategyDefinition.name,
                        oneDayCmcEquityCurve.x.size(),
                        oneDayCmcEquityCurve.y.size()
                    );
                }

                chartHtml = injectBuyAndHoldTraceIntoPlotlyData(
                    std::move(chartHtml),
                    benchmarkPoints,
                    dataset.benchmarkSymbol,
                    initialBalance
                );

                std::error_code removeError;
                std::filesystem::remove(temporaryChartPath, removeError);
                if (removeError) {
                    LG_WARN(
                        "Could not remove temporary chart '{}': {}",
                        temporaryChartPath.string(),
                        removeError.message()
                    );
                }

                if (dataset.id == "1d_cmc") {
                    oneDayCmcStrategyCharts.push_back(
                        CorrelationChartEntry{
                            strategyDefinition.name,
                            std::move(oneDayCmcEquityCurve.x),
                            std::move(oneDayCmcEquityCurve.y)
                        }
                    );

                    if (oneDayCmcBtcPoints.empty()) {
                        oneDayCmcBtcPoints = benchmarkPoints;
                        oneDayCmcBtcSymbol = dataset.benchmarkSymbol;
                    }
                }

                chartEntries.push_back(ChartEntry{
                    dataset.id,
                    dataset.fileName,
                    dataset.benchmarkSymbol,
                    std::move(chartHtml)
                });

                ++successfulRuns;
                run["status"] = "success";

                LG_INFO(
                    "Completed strategy={} dataset={}",
                    strategyDefinition.name,
                    dataset.id
                );
            } catch (const std::exception& exception) {
                ++failedRuns;
                run["status"] = "failed";
                run["failure_reason"] = exception.what();
                LG_ERROR(
                    "Backtest failed for strategy={} dataset={}: {}",
                    strategyDefinition.name,
                    dataset.id,
                    exception.what()
                );
            } catch (...) {
                ++failedRuns;
                run["status"] = "failed";
                run["failure_reason"] = "Unknown non-standard exception";
                LG_ERROR(
                    "Backtest failed for strategy={} dataset={}: "
                    "unknown non-standard exception",
                    strategyDefinition.name,
                    dataset.id
                );
            }
            manifest["runs"].push_back(run);
        }

        if (!chartEntries.empty()) {
            try {
                const std::filesystem::path indexPath =
                    studyDir /
                    (strategyDefinition.outputSlug + "_all_datasets.html");

                writeStrategyIndexHtml(
                    indexPath,
                    strategyDefinition.name,
                    chartEntries
                );

                LG_INFO(
                    "Combined strategy HTML written: {}",
                    indexPath.string()
                );
            } catch (const std::exception& exception) {
                ++failedRuns;
                LG_ERROR(
                    "Could not write combined HTML for {}: {}",
                    strategyDefinition.name,
                    exception.what()
                );
            }
        }
    }

    if (!oneDayCmcStrategyCharts.empty() &&
        !oneDayCmcBtcPoints.empty()) {
        try {
            const std::filesystem::path correlationPath =
                studyDir / "1d_cmc_strategy_equity_correlations.html";

            write1dCmcCorrelationHtml(
                correlationPath,
                oneDayCmcStrategyCharts,
                oneDayCmcBtcPoints,
                oneDayCmcBtcSymbol,
                initialBalance
            );

            LG_INFO(
                "1d_cmc equity/correlation HTML written: {}",
                correlationPath.string()
            );
        } catch (const std::exception& exception) {
            ++failedRuns;
            LG_ERROR(
                "Could not write the 1d_cmc equity/correlation HTML: {}",
                exception.what()
            );
        }
    } else {
        LG_WARN(
            "The 1d_cmc equity/correlation HTML was not created because "
            "no successful 1d_cmc strategy curves or BTC benchmark points "
            "were collected."
        );
    }

    LG_INFO("============================================================");
    LG_INFO(
        "Multi-dataset study completed: {} successful runs, {} failed/skipped runs",
        successfulRuns,
        failedRuns
    );
    LG_INFO("Outputs: {}", studyDir.string());

    manifest["successful_runs"] = successfulRuns;
    manifest["failed_or_skipped_runs"] = failedRuns;
    std::ofstream metadata(studyDir / "study_metadata.json");
    metadata.exceptions(std::ios::failbit | std::ios::badbit);
    metadata << manifest.dump(2) << '\n';
    return successfulRuns > 0U && failedRuns == 0U ? 0 : 1;
}
catch (const std::exception& error)
{
    std::cerr << "Scenario study failed: " << error.what() << '\n';
    return 1;
}
