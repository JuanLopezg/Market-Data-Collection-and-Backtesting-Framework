#include "backtester.h"
#include "database_utils.h"
#include "donchian_signal_strategy.h"
#include "entry_exit_only_rebalance_policy.h"
#include "logger.h"
#include "relative_threshold_rebalance_policy.h"
#include "research_strategy_definitions.h"
#include "sample_covariance_estimator.h"
#include "sensitivity_study.h"
#include "threshold_rebalance_policy.h"
#include "validated/pure_rsi.h"
#include "volatility_target_sizer.h"
#include "xh_breakout_strategy.h"

#include <boost/program_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace research_studies;

// PureRSI (default), DonchianBreakout, XHBreakout and XHBreakout_ATR with their
// full study grids. benchmarkSymbol feeds Donchian's market-state filter.
// --btc-filter-length: PureRSI opens new signals only while the benchmark closes above
// SMA(close, length); 0 = no filter. A study option, so PureRSI CSV columns are unchanged.
unsigned int pureRsiBenchmarkFilterLength = 0;

std::vector<StrategyDefinition> makeHtmlStudyDefinitions(const std::string& benchmarkSymbol)
{
    std::vector<StrategyDefinition> definitions;

    // ------------------------------------------------------------------
    // PureRSI
    //
    // Test the RSI signal itself. Position sizing and maximum positions are
    // intentionally fixed so the sweep measures signal robustness rather
    // than portfolio concentration.
    //
    // Planned grid:
    //   RSI length : 2 to 20, step 1
    //   RSI entry  : 55 to 95, step 2.5
    //   RSI exit   : 25 to 80, step 2.5
    //
    // Invalid combinations where entry <= exit are rejected.
    // ------------------------------------------------------------------
    definitions.push_back(StrategyDefinition{
        "PureRSI",
        {
            {"rsiLength", 7.0},
            {"rsiEntry", 80.0},
            {"rsiExit", 70.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 1000000.0}
        },
        makeSensitivityParameters({
            {"rsiLength", "RSI length", 2.0, 20.0, 1.0, true},
            {"rsiEntry", "RSI entry", 55.0, 95.0, 2.5, true},
            {"rsiExit", "RSI exit", 25.0, 80.0, 2.5, true},
            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int rsiLength = unsignedParameterOr(parameters, "rsiLength", 7U);
            const double rsiEntry = parameterOr(parameters, "rsiEntry", 80.0);
            const double rsiExit = parameterOr(parameters, "rsiExit", 70.0);
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 1000000U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = makeStudyRanker(
                IndicatorSpec{IndicatorKind::RSI, PriceField::Close, rsiLength},
                true
            );

            return std::make_unique<StrategyPureRSI>(
                maxPositionsOpen,
                std::move(universeSelector),
                std::move(ranker),
                maxRankingPosition,
                rsiLength,
                rsiEntry,
                rsiExit,
                makeBenchmarkTrendFilter(benchmarkSymbol, pureRsiBenchmarkFilterLength)
            );
        },
        [](const ParameterValues& parameters) {
            return parameterOr(parameters, "rsiEntry", 0.0) >
                   parameterOr(parameters, "rsiExit", 0.0);
        }
    });


    // ------------------------------------------------------------------
    // DonchianBreakout
    //
    // Main questions:
    //   - How sensitive is the breakout to channel length?
    //   - Does the BTC market-state filter improve robustness?
    //   - Does the momentum ranking horizon matter?
    //
    // The benchmark moving average is also tested when the market-state
    // filter is enabled. Redundant filter-OFF SMA combinations are rejected.
    // ------------------------------------------------------------------
    definitions.push_back(StrategyDefinition{
        "DonchianBreakout",
        {
            {"donchianLookback", 30.0},
            {"useMarketStateFilter", 0.0},
            {"benchmarkMovingAverageLength", 50.0},
            {"momentumLength", 30.0},
            {"quantityPercent", 10.0},
            {"maxPositionsOpen", 10.0},
            {"maxRankingPosition", 9999999.0}
        },
        makeSensitivityParameters({
            {"donchianLookback", "Donchian lookback", 10.0, 150.0, 5.0, true},
            {"useMarketStateFilter", "Market-state filter (0/1)", 0.0, 1.0, 1.0, true},
            {"benchmarkMovingAverageLength", "Benchmark moving-average length", 25.0, 100.0, 5.0, true},
            {"momentumLength", "Momentum length", 10.0, 100.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int donchianLookback = unsignedParameterOr(
                parameters, "donchianLookback", 30U
            );
            const bool useMarketStateFilter =
                parameterOr(parameters, "useMarketStateFilter", 0.0) >= 0.5;
            const unsigned int benchmarkMovingAverageLength = unsignedParameterOr(
                parameters, "benchmarkMovingAverageLength", 50U
            );
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = makeStudyRanker(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, momentumLength},
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
        },
        [](const ParameterValues& parameters) {
            const bool useMarketStateFilter =
                parameterOr(parameters, "useMarketStateFilter", 0.0) >= 0.5;

            if (!useMarketStateFilter) {
                return parameterOr(
                    parameters,
                    "benchmarkMovingAverageLength",
                    50.0
                ) == 50.0;
            }

            return true;
        }
    });


    // ------------------------------------------------------------------
    // XHBreakout
    //
    // Sweep the three parameters that define the trading logic/ranking:
    // breakout lookback, fast exit MA and momentum ranking horizon.
    // ------------------------------------------------------------------
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
            {"xH", "XH lookback", 10.0, 150.0, 5.0, true},
            {"fastMovingAverageLength", "Fast moving-average length", 2.0, 20.0, 1.0, true},
            {"momentumLength", "Momentum length", 10.0, 100.0, 5.0, true},
            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(parameters, "xH", 30U);
            const unsigned int fastMovingAverageLength = unsignedParameterOr(
                parameters, "fastMovingAverageLength", 5U
            );
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = makeStudyRanker(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, momentumLength},
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
        [](const ParameterValues&) { return true; }
    });


    // ------------------------------------------------------------------
    // XHBreakout_ATR
    //
    // Test the breakout horizon together with the two main ATR-stop
    // parameters. Momentum is swept more coarsely because the exit structure
    // is the main difference versus XHBreakout.
    // ------------------------------------------------------------------
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
            {"xH", "XH lookback", 10.0, 150.0, 10.0, true},
            {"atrLength", "ATR length", 5.0, 29.0, 2.0, true},
            {"atrMultiplier", "ATR stop multiplier", 1.0, 6.0, 0.25, true},
            {"momentumLength", "Momentum length", 10.0, 100.0, 10.0, true},
            {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
            {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
        }),
        [=](const ParameterValues& parameters) {
            const unsigned int xH = unsignedParameterOr(parameters, "xH", 30U);
            const unsigned int atrLength = unsignedParameterOr(
                parameters, "atrLength", 14U
            );
            const double atrMultiplier = parameterOr(
                parameters, "atrMultiplier", 3.0
            );
            const unsigned int momentumLength = unsignedParameterOr(
                parameters, "momentumLength", 30U
            );
            const unsigned int maxPositionsOpen = unsignedParameterOr(
                parameters, "maxPositionsOpen", 10U
            );
            const unsigned int maxRankingPosition = unsignedParameterOr(
                parameters, "maxRankingPosition", 9999999U
            );

            auto universeSelector = makeTopLiquidityUniverse();
            auto ranker = makeStudyRanker(
                IndicatorSpec{IndicatorKind::ROC, PriceField::Close, momentumLength},
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
            return parameterOr(parameters, "atrMultiplier", 0.0) > 0.0 &&
                   parameterOr(parameters, "atrLength", 0.0) > 0.0;
        }
    });


    return definitions;
}


// Position sizing for every run of the study, chosen once on the command line.
// equal-weight: fixed quantityPercent per signal, entry/exit-only rebalance (default).
// volatility-target: signal shape scaled to an annualized portfolio volatility from a
// sample covariance. --rebalance chooses when a held position is resized to its target:
// none (entries/exits only), absolute (weight drift beyond --rebalance-threshold points) or
// relative (drift beyond that fraction of the current weight). Default: none for equal
// weight, absolute for volatility target. The deployed config/portfolio/pure_rsi_vol_target.json
// uses the volatility target with a 20% relative threshold since 2026-10-15.
struct StudySizing {
    std::string sizer = "equal-weight";
    std::string rebalance = "default";
    double targetVolatility = 0.20;
    std::size_t covarianceLookback = 5;
    double periodsPerYear = 365.0;
    double rebalanceThreshold = 0.02;
};

StudySizing studySizing;

std::unique_ptr<RebalancePolicy> makeStudyRebalancePolicy()
{
    const std::string rule = studySizing.rebalance != "default" ? studySizing.rebalance
        : studySizing.sizer == "volatility-target" ? "absolute" : "none";
    if (rule == "relative")
        return std::make_unique<RelativeThresholdRebalancePolicy>(studySizing.rebalanceThreshold);
    if (rule == "absolute")
        return std::make_unique<ThresholdRebalancePolicy>(studySizing.rebalanceThreshold);
    return std::make_unique<EntryExitOnlyRebalancePolicy>();
}

StrategyPortfolio makeSizedStudyPortfolio(
    const StrategyDefinition& definition,
    const ParameterValues& parameters
)
{
    // The study allocates all capital to one strategy. Signal strength and
    // fixed asset weight are separate; execution quantities resolve at next open.
    StrategyPortfolio portfolio;
    if (studySizing.sizer == "volatility-target") {
        portfolio.emplace_back(
            1,
            definition.create(parameters),
            1.0,
            std::make_unique<VolatilityTargetSizer>(
                std::make_unique<SampleCovarianceEstimator>(
                    studySizing.covarianceLookback, studySizing.periodsPerYear),
                studySizing.targetVolatility),
            RiskConstraints(1.5, 1.5),
            makeStudyRebalancePolicy()
        );
        return portfolio;
    }

    const double assetWeight = parameterOr(parameters, "quantityPercent", 10.0) / 100.0;
    portfolio.emplace_back(
        1,
        definition.create(parameters),
        1.0,
        std::make_unique<EqualWeightSizer>(assetWeight),
        RiskConstraints(1.5, 1.5),
        makeStudyRebalancePolicy()
    );
    return portfolio;
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
    std::vector<std::string> parameterOverrides;
    std::string rankerChoice;
    Timestamp startDate;
    Timestamp endDate;
    double feeTaker;
    options::options_description arguments("HTML research study options");
    arguments.add_options()
        ("help,h", "Show study options")
        ("database", options::value(&databasePath)->default_value("storage/databases/1d_cmc.csv"), "Input CSV/SQLite")
        ("start", options::value(&startDate)->default_value(20200101), "Cold start YYYYMMDD (inclusive)")
        ("end", options::value(&endDate)->default_value(20200416), "Cutoff YYYYMMDD (inclusive)")
        ("timeframe", options::value(&databaseTimeframe)->default_value("1d"), "Annualization timeframe")
        ("benchmark", options::value(&benchmarkSymbol)->default_value("BTC"), "Annualization benchmark")
        ("study-id", options::value(&studyId)->default_value(""), "New study directory name (default UTC timestamp)")
        ("output-root", options::value(&outputRoot)->default_value("storage/backtests/sensitivity_results"), "Study parent directory")
        ("strategy", options::value(&selectedStrategy)->default_value("PureRSI"), "Strategy name or all")
        ("ranker", options::value(&rankerChoice)->default_value("own"), "own (each strategy's ranker) or volume (SMA(volume, 25), highest first; PureMom keeps momentum)")
        ("set", options::value(&parameterOverrides)->composing(), "Baseline parameter name=value (repeatable, e.g. --set rsiExit=65)")
        ("grid", options::value(&grid)->default_value("none"), "none, compact or full sensitivity grid")
        ("fee-taker", options::value(&feeTaker)->default_value(0.0), "Market commission fraction per fill")
        ("sizer", options::value(&studySizing.sizer)->default_value("equal-weight"), "equal-weight or volatility-target")
        ("target-volatility", options::value(&studySizing.targetVolatility)->default_value(0.20), "volatility-target: annualized target (0.20 = 20%)")
        ("covariance-lookback", options::value(&studySizing.covarianceLookback)->default_value(5), "volatility-target: returns in the sample covariance")
        ("rebalance-threshold", options::value(&studySizing.rebalanceThreshold)->default_value(0.02), "volatility-target: weight drift that triggers a resize")
        ("rebalance", options::value(&studySizing.rebalance)->default_value("default"), "none, absolute (weight points) or relative (fraction of the current weight); default: none for equal-weight, absolute for volatility-target")
        ("btc-filter-length", options::value(&pureRsiBenchmarkFilterLength)->default_value(0), "PureRSI: new signals only while the benchmark closes above SMA(close, length); 0 = off");
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
        if (studySizing.sizer != "equal-weight" && studySizing.sizer != "volatility-target")
            throw std::invalid_argument("Sizer must be equal-weight or volatility-target");
        if (rankerChoice != "own" && rankerChoice != "volume")
            throw std::invalid_argument("Ranker must be own or volume");
        if (studySizing.rebalance != "default" && studySizing.rebalance != "none" &&
            studySizing.rebalance != "absolute" && studySizing.rebalance != "relative")
            throw std::invalid_argument("Rebalance must be none, absolute or relative");
        if (!std::isfinite(studySizing.targetVolatility) || studySizing.targetVolatility <= 0.0 ||
            studySizing.covarianceLookback < 2U ||
            !std::isfinite(studySizing.rebalanceThreshold) || studySizing.rebalanceThreshold < 0.0)
            throw std::invalid_argument("Volatility-target settings must be positive (lookback >= 2)");
        if (studyId.empty()) {
            studyId = utcTimestamp();
            studyId.erase(std::remove(studyId.begin(), studyId.end(), ':'), studyId.end());
            studyId = "current_html_" + studyId;
        }
        if (studyId == "." || studyId == ".." || studyId.find_first_of("/\\") != std::string::npos)
            throw std::invalid_argument("Study ID must be a single directory name");
    } catch (const std::exception& exception) {
        LG_ERROR("Invalid study options: {}", exception.what());
        return 1;
    }

    Logger::Instance().Setup(true, false); // DEBUG on, stdout only

    double periodsPerYear;
    try {
        periodsPerYear = periodsPerYearFromTimeframe(databaseTimeframe);
    } catch (const std::exception& exception) {
        LG_ERROR("{}", exception.what());
        return 1;
    }

    studySizing.periodsPerYear = periodsPerYear;
    const std::string rebalanceRule = studySizing.rebalance != "default" ? studySizing.rebalance
        : studySizing.sizer == "volatility-target" ? "absolute" : "none";
    if (studySizing.sizer == "volatility-target")
        LG_INFO("Sizer: volatility-target {:.0f}% annualized, covariance of {} returns, max gross 1.5x",
                studySizing.targetVolatility * 100.0, studySizing.covarianceLookback);
    else
        LG_INFO("Sizer: equal-weight (quantityPercent per signal)");
    if (rebalanceRule == "none")
        LG_INFO("Rebalance: entries and exits only");
    else
        LG_INFO("Rebalance: resize at {:.1f}% {} drift", studySizing.rebalanceThreshold * 100.0, rebalanceRule);
    if (pureRsiBenchmarkFilterLength > 0)
        LG_INFO("PureRSI filter: new signals only while {} closes above SMA({})", benchmarkSymbol, pureRsiBenchmarkFilterLength);

    constexpr double initialBalance = 100000.0;

    // Zero-fee defaults follow the supported PureRSI research configuration.
    constexpr double feeMaker = 0.0;
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

    if (std::filesystem::exists(studyDirectory)) {
        LG_ERROR("Study directory already exists: {}. Choose a new study ID.", studyDirectory.string());
        return 1;
    }

    if (!ensureDirectoryExists(studyDirectory) ||
        !ensureDirectoryExists(reportsDirectory)) {
        return 1;
    }

    LG_INFO("Database loading started");
    OHLCVData ohlcvData;
    try {
        if (!std::filesystem::is_regular_file(databasePath))
            throw std::runtime_error("Input database does not exist: " + databasePath);
        ohlcvData = loadDatabase(databasePath, startDate, endDate);
        if (ohlcvData.data.empty())
            throw std::runtime_error("No market data in the selected window");
        if (!ohlcvData.data.contains(benchmarkSymbol))
            throw std::runtime_error("Annualization benchmark is absent from the selected window");
    } catch (const std::exception& exception) {
        LG_ERROR("Study input failed: {}", exception.what());
        return 1;
    }
    LG_INFO("Database loaded successfully");
    LG_INFO("Annualization: {} bars, benchmark {}, {} periods per year", databaseTimeframe, benchmarkSymbol, periodsPerYear);

    rankByVolumeAverage() = rankerChoice == "volume";
    if (rankByVolumeAverage())
        LG_INFO("Ranking: SMA(volume, 25), highest first (PureMom keeps its momentum ranking)");
    std::vector<StrategyDefinition> strategyDefinitions = makeHtmlStudyDefinitions(benchmarkSymbol);
    // Timed research strategies, only when named explicitly (keeps "all" to the four HTML
    // definitions), with the values of the multi-strategy study where the shared
    // definitions differ (ATRBreakout also matches the parameters frozen in August 2026).
    const std::map<std::string, ParameterValues> multiStrategyValues{
        {"PureMom", {}},
        {"BargainChaser", {{"barsUntilExit", 1.0}}},
        {"MRRSILong", {}},
        {"ATRBreakout", {{"heldBars", 3.0}, {"atrMultiple", 0.875}, {"atrLength", 10.0}}},
        {"MRShort", {{"rsiEntry", 65.0}, {"entryAtrLength", 10.0}, {"heldBars", 7.0}}},
    };
    if (const auto values = multiStrategyValues.find(selectedStrategy); values != multiStrategyValues.end()) {
        for (auto& definition : makeStrategyDefinitions(benchmarkSymbol)) {
            if (definition.name != selectedStrategy)
                continue;
            for (const auto& [key, value] : values->second)
                definition.currentParameters[key] = value;
            strategyDefinitions.push_back(std::move(definition));
        }
    }

    if (selectedStrategy != "all") {
        std::erase_if(strategyDefinitions, [&](const auto& definition) {
            return definition.name != selectedStrategy;
        });
    }
    if (strategyDefinitions.empty()) {
        LG_ERROR("Strategy is unavailable in this target: {}", selectedStrategy);
        return 1;
    }
    // --set replaces baseline values of the selected strategies; grid ranges are unchanged.
    for (const std::string& override : parameterOverrides) {
        const auto separator = override.find('=');
        double value = 0.0;
        try {
            if (separator == std::string::npos || separator == 0)
                throw std::invalid_argument("missing name=value");
            std::size_t parsed = 0;
            value = std::stod(override.substr(separator + 1), &parsed);
            if (parsed != override.size() - separator - 1 || !std::isfinite(value))
                throw std::invalid_argument("value is not a number");
        } catch (const std::exception&) {
            LG_ERROR("Invalid --set {} (expected name=number)", override);
            return 1;
        }
        const std::string key = override.substr(0, separator);
        bool applied = false;
        for (auto& definition : strategyDefinitions) {
            if (!definition.currentParameters.contains(key))
                continue;
            definition.currentParameters[key] = value;
            applied = true;
            LG_INFO("Baseline {} {} = {}", definition.name, key, value);
        }
        if (!applied) {
            LG_ERROR("--set {}: no selected strategy has parameter {}", override, key);
            return 1;
        }
    }
    for (const auto& definition : strategyDefinitions) {
        if (!definition.isValidCombination(definition.currentParameters)) {
            LG_ERROR("Baseline parameters of {} are not a valid combination", definition.name);
            return 1;
        }
    }
    if (grid == "compact") {
        // Bounded signal study: 2 lengths x 2 entries x 2 exits = 8 runs.
        // Include the baseline and reject entry <= exit through the normal CSV path.
        for (auto& definition : strategyDefinitions) {
            if (definition.name != "PureRSI") {
                for (auto& parameter : definition.sensitivityParameters) {
                    parameter.enabled = false;
                    parameter.minimum = parameterOr(definition.currentParameters, parameter.key, 0.0);
                    parameter.maximum = parameter.minimum;
                    parameter.spacing = 1.0;
                    if (parameter.key == "xH" || parameter.key == "fastMovingAverageLength" ||
                        parameter.key == "atrLength" || parameter.key == "donchianLookback" ||
                        parameter.key == "useMarketStateFilter") {
                        parameter.enabled = true;
                        parameter.maximum += 1.0;
                    }
                    if (parameter.key == "atrMultiplier") {
                        parameter.enabled = true;
                        parameter.spacing = 0.5;
                        parameter.maximum += 0.5;
                    }
                }
                continue;
            }
            definition.sensitivityParameters = makeSensitivityParameters({
                {"rsiLength", "RSI length", 7.0, 8.0, 1.0, true},
                {"rsiEntry", "RSI entry", 70.0, 80.0, 10.0, true},
                {"rsiExit", "RSI exit", 60.0, 70.0, 10.0, true},
                {"quantityPercent", "Quantity (%)", 10.0, 10.0, 1.0, false},
                {"maxPositionsOpen", "Maximum open positions", 10.0, 10.0, 1.0, false}
            });
        }
    }
    std::ofstream executionSettings(studyDirectory / "execution_settings.csv");
    executionSettings.exceptions(std::ios::failbit | std::ios::badbit);
    executionSettings << "runtime,start,end,grid,cutoff_policy\n";
    executionSettings << (selectedStrategy == "XHBreakout" || selectedStrategy == "XHBreakout_ATR" || selectedStrategy == "all"
        ? "current-lib-stop-entry," : "current-lib-next-open,");
    executionSettings << startDate << ',' << endDate << ',' << grid << ",mark-open-positions\n";

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
        .makePortfolio = makeSizedStudyPortfolio,
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

    // A failed grid run is recorded in the CSV and also fails the process.
    return std::any_of(results.sensitivityStats.begin(), results.sensitivityStats.end(), [](const auto& entry) {
        return entry.second.failedRunCount > 0;
    }) ? 1 : 0;
}
catch (const std::exception& exception)
{
    LG_ALERT("Research study failed: {}", exception.what());
    return 1;
}
