#include "sensitivity_study.h"

#include "backtest_html_report.h"
#include "backtester.h"
#include "logger.h"

#include <cmath>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace research_studies {

std::string formatCsvNumber(double value)
{
    if (!std::isfinite(value)) {
        return {};
    }

    std::ostringstream output;
    output << std::setprecision(15) << value;
    return output.str();
}

void writeCsvRow(std::ostream& output, const std::vector<std::string>& cells)
{
    for (std::size_t index = 0U; index < cells.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }

        output << '"';
        for (const char character : cells[index]) {
            if (character == '"') {
                output << "\"\"";
            } else {
                output << character;
            }
        }
        output << '"';
    }
    output << '\n';
}

std::string utcTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm utcTime{};

#if defined(_WIN32)
    gmtime_s(&utcTime, &now);
#else
    gmtime_r(&now, &utcTime);
#endif

    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool ensureDirectoryExists(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);

    if (error) {
        LG_ERROR(
            "Could not create output directory '{}': {}",
            path.string(),
            error.message()
        );
        return false;
    }

    return true;
}

double periodsPerYearFromTimeframe(const std::string& timeframe)
{
    if (timeframe == "1d") {
        return 365.0;
    }

    if (timeframe.size() >= 2U && timeframe.back() == 'h') {
        const int hours = std::stoi(timeframe.substr(0, timeframe.size() - 1U));

        if (
            hours == 1 ||
            hours == 2 ||
            hours == 3 ||
            hours == 4 ||
            hours == 6 ||
            hours == 8 ||
            hours == 12
        ) {
            return 365.0 * 24.0 / static_cast<double>(hours);
        }
    }

    throw std::runtime_error(
        "Unsupported database timeframe: " + timeframe +
        ". Use 1d, 1h, 2h, 3h, 4h, 6h, 8h or 12h."
    );
}

SensitivityCsvWriter::SensitivityCsvWriter(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const StrategyDefinition& definition
)
    : output_(outputPath),
      studyId_(studyId),
      strategyName_(definition.name)
{
    if (!output_.is_open()) {
        throw std::runtime_error(
            "Could not write sensitivity CSV: " + outputPath.string()
        );
    }
    output_.exceptions(std::ios::failbit | std::ios::badbit);

    parameterKeys_.reserve(definition.currentParameters.size());
    for (const auto& [key, value] : definition.currentParameters) {
        parameterKeys_.push_back(key);
        (void)value;
    }

    std::vector<std::string> header{
        "study_id",
        "strategy",
        "run_id",
        "status",
        "failure_reason"
    };
    header.insert(header.end(), parameterKeys_.begin(), parameterKeys_.end());

    const std::vector<std::string> metricHeaders{
        "net_return_percent",
        "annualized_return_percent",
        "max_drawdown_percent",
        "sharpe_ratio",
        "sortino_ratio",
        "calmar_ratio",
        "profit_factor",
        "expectancy_per_trade",
        "trade_count",
        "exposure_percent",
        "turnover_multiple",
        "win_rate_percent",
        "average_win",
        "average_loss",
        "max_consecutive_losses",
        "worst_loss_streak",
        "average_holding_bars",
        "net_profit",
        "final_equity"
    };
    header.insert(header.end(), metricHeaders.begin(), metricHeaders.end());

    writeCsvRow(output_, header);
}

void SensitivityCsvWriter::writeRun(
    std::size_t runId,
    const std::string& status,
    const std::string& failureReason,
    const ParameterValues& parameters,
    const BacktestMetrics* metrics
)
{
    std::vector<std::string> row{
        studyId_,
        strategyName_,
        std::to_string(runId),
        status,
        failureReason
    };

    for (const std::string& key : parameterKeys_) {
        const auto iterator = parameters.find(key);
        row.push_back(
            iterator == parameters.end()
                ? std::string{}
                : formatCsvNumber(iterator->second)
        );
    }

    if (metrics != nullptr) {
        row.insert(
            row.end(),
            {
                formatCsvNumber(metrics->netReturnPercent),
                formatCsvNumber(metrics->annualizedReturnPercent),
                formatCsvNumber(metrics->maxDrawdownPercent),
                formatCsvNumber(metrics->sharpeRatio),
                formatCsvNumber(metrics->sortinoRatio),
                formatCsvNumber(metrics->calmarRatio),
                formatCsvNumber(metrics->profitFactor),
                formatCsvNumber(metrics->expectancyPerTrade),
                std::to_string(metrics->tradeCount),
                formatCsvNumber(metrics->exposurePercent),
                formatCsvNumber(metrics->turnoverMultiple),
                formatCsvNumber(metrics->winRatePercent),
                formatCsvNumber(metrics->averageWin),
                formatCsvNumber(metrics->averageLoss),
                std::to_string(metrics->maximumConsecutiveLosses),
                formatCsvNumber(metrics->worstConsecutiveLossPnl),
                formatCsvNumber(metrics->averageHoldingBars),
                formatCsvNumber(metrics->netProfit),
                formatCsvNumber(metrics->finalEquity)
            }
        );
    } else {
        constexpr std::size_t metricColumnCount = 19U;
        row.insert(row.end(), metricColumnCount, std::string{});
    }

    writeCsvRow(output_, row);
}

std::string invalidCombinationReason(
    const StrategyDefinition& definition,
    const ParameterValues& parameters
)
{
    for (const auto& [key, value] : parameters) {
        if (!std::isfinite(value)) {
            return "Non-finite parameter value for '" + key + "'";
        }
    }

    if (definition.name == "PureRSI" &&
        parameterOr(parameters, "rsiEntry", 0.0) <=
        parameterOr(parameters, "rsiExit", 0.0)) {
        return "rsiEntry must be strictly greater than rsiExit";
    }

    if (definition.isValidCombination &&
        !definition.isValidCombination(parameters)) {
        return "Rejected by the strategy-specific valid-combination predicate";
    }

    return {};
}

BacktestMetrics runSingleBacktest(
    const StrategyDefinition& definition,
    const ParameterValues& parameters,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const BacktestMetricsSettings& metricsSettings
)
{
    BacktestContext context(
        ohlcvData,
        settings.makePortfolio(definition, parameters),
        settings.initialBalance,
        settings.feeTaker
    );

    Backtester tester(context);
    tester.loop();

    return calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        settings.initialBalance,
        metricsSettings
    );
}

ParameterSensitivityExecution runParameterSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& csvPath,
    const std::string& studyId,
    SensitivityProgressReporter& progress
)
{
    ParameterSensitivityExecution execution;

    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition, settings.keepEndpointsOnly);

    if (active.empty()) {
        return execution;
    }

    // Sensitivity runs use historical metrics only. Monte Carlo is disabled
    // here so every grid point remains a single deterministic backtest.
    BacktestMetricsSettings sensitivityMetricsSettings = settings.metricsSettings;
    sensitivityMetricsSettings.monteCarloSimulationCount = 0U;
    sensitivityMetricsSettings.monteCarloFanPointCount = 0U;

    SensitivityAggregator aggregator(active);
    SensitivityCsvWriter csvWriter(csvPath, studyId, definition);

    ParameterValues parameters = definition.currentParameters;
    std::size_t runId = 0U;
    progress.setStrategy(definition.name);

    auto processCombination = [&](const ParameterValues& combination) {
        ++runId;
        ++execution.stats.plannedRunCount;

        const std::string validationReason =
            invalidCombinationReason(definition, combination);

        if (!validationReason.empty()) {
            ++execution.stats.invalidRunCount;
            csvWriter.writeRun(runId, "invalid", validationReason, combination, nullptr);
            progress.advance();
            return;
        }

        try {
            const BacktestMetrics metrics = runSingleBacktest(
                definition,
                combination,
                ohlcvData,
                settings,
                sensitivityMetricsSettings
            );

            csvWriter.writeRun(runId, "success", {}, combination, &metrics);
            ++execution.stats.successfulRunCount;
            aggregator.add(combination, metrics);
        } catch (const std::exception& exception) {
            ++execution.stats.failedRunCount;
            csvWriter.writeRun(runId, "failed", exception.what(), combination, nullptr);
            LG_ERROR(
                "Sensitivity run failed for {} (run {}): {}",
                definition.name,
                runId,
                exception.what()
            );
        } catch (...) {
            ++execution.stats.failedRunCount;
            csvWriter.writeRun(runId, "failed", "Unknown non-standard exception", combination, nullptr);
            LG_ERROR(
                "Sensitivity run failed for {} (run {}): unknown exception",
                definition.name,
                runId
            );
        }

        progress.advance();
    };

    enumerateSensitivityCombinations(active, 0U, parameters, processCombination);

    execution.reports = aggregator.reports();
    return execution;
}

StrategyExecutionResult runStrategyAndWriteReport(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& reportPath,
    const std::filesystem::path& sensitivityCsvPath,
    const std::string& studyId,
    SensitivityProgressReporter* progress
)
{
    StrategyExecutionResult result;

    BacktestContext context(
        ohlcvData,
        settings.makePortfolio(definition, definition.currentParameters),
        settings.initialBalance,
        settings.feeTaker
    );

    Backtester tester(context);
    tester.loop();

    result.metrics = calculateBacktestMetrics(
        definition.name,
        context.GetTradesHistory(),
        context.GetBalanceEquityHistoric(),
        context.GetMarketData(),
        settings.initialBalance,
        settings.metricsSettings
    );

    const auto reportDirectory = reportPath.parent_path();
    tester.storeTradesCSV(reportDirectory / (definition.name + "_trades.csv"));
    std::ofstream accountOutput(reportDirectory / (definition.name + "_account.csv"));
    accountOutput.exceptions(std::ios::failbit | std::ios::badbit);
    accountOutput << "timestamp,cash,balance,equity\n" << std::setprecision(17);
    for (const auto& snapshot : context.GetAccountHistory()) {
        accountOutput << snapshot.timestamp << ',' << snapshot.cash << ','
                      << snapshot.balance << ',' << snapshot.equity << '\n';
    }
    SensitivityCsvWriter baselineMetrics(
        reportDirectory / (definition.name + "_metrics.csv"), studyId, definition
    );
    baselineMetrics.writeRun(0, "success", {}, definition.currentParameters, &result.metrics);

    std::vector<ParameterSensitivityReport> parameterSensitivity;
    if (progress != nullptr) {
        ParameterSensitivityExecution sensitivity = runParameterSensitivity(
            definition,
            ohlcvData,
            settings,
            sensitivityCsvPath,
            studyId,
            *progress
        );
        parameterSensitivity = std::move(sensitivity.reports);
        result.sensitivityStats = sensitivity.stats;
    }

    if (!writeBacktestHtmlReport(
            reportPath,
            result.metrics,
            context.GetBalanceEquityHistoric(),
            context.GetMarketData(),
            parameterSensitivity
        )) {
        throw std::runtime_error("Could not write report for strategy " + definition.name);
    }

    return result;
}

StudyResults runStudyStrategies(
    const std::vector<StrategyDefinition>& definitions,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    bool runGrid,
    const std::filesystem::path& studyDirectory,
    const std::filesystem::path& reportsDirectory,
    const std::string& studyId
)
{
    std::size_t totalSensitivityRuns = 0U;
    if (runGrid) {
        for (const StrategyDefinition& definition : definitions) {
            const std::size_t strategyRuns =
                countSensitivityCombinations(definition, settings.keepEndpointsOnly);
            totalSensitivityRuns += strategyRuns;
            LG_INFO(
                "Sensitivity grid {}: {} planned parameter combinations",
                definition.name,
                strategyRuns
            );
        }
        LG_INFO(
            "Total parameter-sensitivity grid: {} planned parameter combinations",
            totalSensitivityRuns
        );
    }

    std::unique_ptr<SensitivityProgressReporter> progress;
    if (runGrid && totalSensitivityRuns > 0U) {
        progress = std::make_unique<SensitivityProgressReporter>(totalSensitivityRuns);
    }

    StudyResults results;
    results.sensitivityStats.reserve(definitions.size());

    try {
        for (const StrategyDefinition& definition : definitions) {
            LG_INFO("Running isolated backtest for {}", definition.name);

            const StrategyExecutionResult result = runStrategyAndWriteReport(
                definition,
                ohlcvData,
                settings,
                reportsDirectory / (definition.name + ".html"),
                studyDirectory / (definition.name + ".csv"),
                studyId,
                progress.get()
            );

            if (progress) {
                std::cout << '\n';
            }
            logBacktestMetrics(result.metrics);
            results.baselineMetrics.insert_or_assign(definition.name, result.metrics);
            results.sensitivityStats.emplace_back(definition.name, result.sensitivityStats);

            if (runGrid) {
                LG_INFO(
                    "{} sensitivity: {} success, {} invalid, {} failed",
                    definition.name,
                    result.sensitivityStats.successfulRunCount,
                    result.sensitivityStats.invalidRunCount,
                    result.sensitivityStats.failedRunCount
                );
            }
        }
    } catch (...) {
        if (progress) {
            progress->finish();
        }
        throw;
    }

    if (progress) {
        progress->finish();
    }
    return results;
}

void logStudySummary(const StudyResults& results, const std::filesystem::path& studyDirectory)
{
    LG_INFO(
        "Completed {} baseline strategy backtests. Study outputs are in: {}",
        results.baselineMetrics.size(),
        studyDirectory.string()
    );

    for (const auto& [strategyName, metrics] : results.baselineMetrics) {
        LG_INFO("Summary {}: return {:.2f}%, annualized {:.2f}%, max drawdown {:.2f}%, Sharpe {:.3f}, Sortino {:.3f}, Calmar {:.3f}, profit factor {:.3f}, {} trades",
                strategyName, metrics.netReturnPercent, metrics.annualizedReturnPercent, metrics.maxDrawdownPercent,
                metrics.sharpeRatio, metrics.sortinoRatio, metrics.calmarRatio, metrics.profitFactor, metrics.tradeCount);
    }
}

void writeParameterGridCsv(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const std::vector<StrategyDefinition>& definitions
)
{
    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write parameter grid CSV: " + outputPath.string()
        );
    }
    output.exceptions(std::ios::failbit | std::ios::badbit);

    writeCsvRow(
        output,
        {
            "study_id",
            "strategy",
            "parameter_name",
            "display_name",
            "current_value",
            "enabled",
            "minimum",
            "maximum",
            "spacing"
        }
    );

    for (const StrategyDefinition& definition : definitions) {
        for (const auto& [key, currentValue] : definition.currentParameters) {
            const SensitivityParameterDefinition* sensitivityDefinition = nullptr;
            for (const SensitivityParameterDefinition& parameter : definition.sensitivityParameters) {
                if (parameter.key == key) {
                    sensitivityDefinition = &parameter;
                    break;
                }
            }

            writeCsvRow(
                output,
                {
                    studyId,
                    definition.name,
                    key,
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : sensitivityDefinition->displayName,
                    formatCsvNumber(currentValue),
                    sensitivityDefinition != nullptr &&
                        sensitivityDefinition->enabled
                        ? "yes"
                        : "no",
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->minimum),
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->maximum),
                    sensitivityDefinition == nullptr
                        ? std::string{}
                        : formatCsvNumber(sensitivityDefinition->spacing)
                }
            );
        }
    }
}

void writeStudyMetadataCsv(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const std::string& databasePath,
    double initialBalance,
    double feeMaker,
    double feeTaker,
    double commissionEntryFactor,
    double commissionExitFactor,
    const BacktestMetricsSettings& metricsSettings,
    bool includeAnnualizationColumns,
    const std::vector<std::pair<std::string, SensitivityRunStats>>& strategyStats
)
{
    std::ofstream output(outputPath);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not write study metadata CSV: " + outputPath.string()
        );
    }
    output.exceptions(std::ios::failbit | std::ios::badbit);

    std::vector<std::string> header{
        "record_type",
        "study_id",
        "created_utc",
        "database_path",
        "initial_balance",
        "fee_maker",
        "fee_taker",
        "commission_entry_factor",
        "commission_exit_factor",
        "periods_per_year"
    };
    if (includeAnnualizationColumns) {
        header.insert(header.end(), {"database_timeframe", "annualization_benchmark_symbol"});
    }
    header.insert(
        header.end(),
        {
            "exclude_simulated_trades",
            "sensitivity_monte_carlo_enabled",
            "strategy",
            "planned_runs",
            "successful_runs",
            "invalid_runs",
            "failed_runs"
        }
    );
    writeCsvRow(output, header);

    const std::string createdUtc = utcTimestamp();

    // Columns shared by the study row and every strategy row, up to the strategy name.
    auto settingsCells = [&](const std::string& recordType) {
        std::vector<std::string> cells{
            recordType,
            studyId,
            createdUtc,
            databasePath,
            formatCsvNumber(initialBalance),
            formatCsvNumber(feeMaker),
            formatCsvNumber(feeTaker),
            formatCsvNumber(commissionEntryFactor),
            formatCsvNumber(commissionExitFactor),
            formatCsvNumber(metricsSettings.periodsPerYear)
        };
        if (includeAnnualizationColumns) {
            cells.insert(
                cells.end(),
                {metricsSettings.databaseTimeframe, metricsSettings.annualizationBenchmarkSymbol}
            );
        }
        cells.insert(
            cells.end(),
            {metricsSettings.excludeSimulatedTrades ? "yes" : "no", "no"}
        );
        return cells;
    };

    std::vector<std::string> studyRow = settingsCells("study");
    studyRow.insert(studyRow.end(), 5U, std::string{});
    writeCsvRow(output, studyRow);

    for (const auto& [strategyName, stats] : strategyStats) {
        std::vector<std::string> row = settingsCells("strategy");
        row.insert(
            row.end(),
            {
                strategyName,
                std::to_string(stats.plannedRunCount),
                std::to_string(stats.successfulRunCount),
                std::to_string(stats.invalidRunCount),
                std::to_string(stats.failedRunCount)
            }
        );
        writeCsvRow(output, row);
    }
}

} // namespace research_studies
