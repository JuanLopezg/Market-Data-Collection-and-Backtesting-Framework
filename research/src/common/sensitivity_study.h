#pragma once

#include "backtest_metrics.h"
#include "parameter_sweep.h"
#include "research_strategy_definitions.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Study driver shared by the HTML runner, the BTC moving-average study and the XH
// robustness study. Every grid point is an isolated backtest; each run gets one row
// in the per-strategy sensitivity CSV, with invalid and failed runs kept (reason,
// empty metric cells) so the CSV accounts for the whole planned grid. Monte Carlo
// runs only for the baseline report, never per grid point.
namespace research_studies {

struct SensitivityRunStats {
    std::size_t plannedRunCount = 0U;
    std::size_t successfulRunCount = 0U;
    std::size_t invalidRunCount = 0U;
    std::size_t failedRunCount = 0U;
};

struct ParameterSensitivityExecution {
    std::vector<ParameterSensitivityReport> reports;
    SensitivityRunStats stats;
};

struct StrategyExecutionResult {
    BacktestMetrics metrics;
    SensitivityRunStats sensitivityStats;
};

// Builds the one-strategy portfolio (sizer, risk limits, rebalance policy) for a
// parameter combination. Each study chooses its own sizing.
using StudyPortfolioFactory =
    std::function<StrategyPortfolio(const StrategyDefinition&, const ParameterValues&)>;

// Settings shared by every run of one study. The studies submit market orders
// only, so the taker fee is the only commission applied to fills.
struct StudyRunSettings {
    StudyPortfolioFactory makePortfolio;
    double initialBalance = 0.0;
    double feeTaker = 0.0;
    BacktestMetricsSettings metricsSettings;
    bool keepEndpointsOnly = false; // see activeSensitivityParameters()
};

// 15 significant digits; empty for a non-finite value.
std::string formatCsvNumber(double value);
// Every cell quoted, embedded quotes doubled.
void writeCsvRow(std::ostream& output, const std::vector<std::string>& cells);
// Wall-clock now as YYYY-MM-DDTHH:MM:SSZ (default study IDs and metadata only).
std::string utcTimestamp();
bool ensureDirectoryExists(const std::filesystem::path& path);
// 1d -> 365, Nh -> 365 * 24 / N for N in {1, 2, 3, 4, 6, 8, 12}; throws otherwise.
double periodsPerYearFromTimeframe(const std::string& timeframe);

// Per-run sensitivity CSV: identity, status, every current parameter key, then 19
// metric columns (empty unless the run succeeded).
class SensitivityCsvWriter {
public:
    SensitivityCsvWriter(
        const std::filesystem::path& outputPath,
        const std::string& studyId,
        const StrategyDefinition& definition
    );

    void writeRun(
        std::size_t runId,
        const std::string& status,
        const std::string& failureReason,
        const ParameterValues& parameters,
        const BacktestMetrics* metrics
    );

private:
    std::ofstream output_;
    std::string studyId_;
    std::string strategyName_;
    std::vector<std::string> parameterKeys_;
};

// Empty when the combination may run; otherwise the reason recorded in the CSV.
std::string invalidCombinationReason(
    const StrategyDefinition& definition,
    const ParameterValues& parameters
);

BacktestMetrics runSingleBacktest(
    const StrategyDefinition& definition,
    const ParameterValues& parameters,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const BacktestMetricsSettings& metricsSettings
);

ParameterSensitivityExecution runParameterSensitivity(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& csvPath,
    const std::string& studyId,
    SensitivityProgressReporter& progress
);

// Baseline run with the current parameters: <name>_trades.csv, <name>_account.csv
// and <name>_metrics.csv next to the HTML report, then the sensitivity grid when
// progress is set, then the HTML report.
StrategyExecutionResult runStrategyAndWriteReport(
    const StrategyDefinition& definition,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    const std::filesystem::path& reportPath,
    const std::filesystem::path& sensitivityCsvPath,
    const std::string& studyId,
    SensitivityProgressReporter* progress
);

struct StudyResults {
    std::map<std::string, BacktestMetrics> baselineMetrics; // by strategy name
    std::vector<std::pair<std::string, SensitivityRunStats>> sensitivityStats; // run order
};

// Runs every strategy in order: <name>.html under reportsDirectory and, when runGrid
// is set, the grid in <name>.csv under studyDirectory, with one progress bar for the
// whole study. Throws when a run or an output fails.
StudyResults runStudyStrategies(
    const std::vector<StrategyDefinition>& definitions,
    const OHLCVData& ohlcvData,
    const StudyRunSettings& settings,
    bool runGrid,
    const std::filesystem::path& studyDirectory,
    const std::filesystem::path& reportsDirectory,
    const std::string& studyId
);

// Final "Completed ..." line and one summary line per strategy, by name.
void logStudySummary(const StudyResults& results, const std::filesystem::path& studyDirectory);

void writeParameterGridCsv(
    const std::filesystem::path& outputPath,
    const std::string& studyId,
    const std::vector<StrategyDefinition>& definitions
);

// One "study" row and one row per strategy with its run counters. The commission
// factor columns are kept for the CSV contract and are always zero: commissions
// come from actual taker fills. includeAnnualizationColumns adds database_timeframe
// and annualization_benchmark_symbol (HTML and XH studies; the BTC study is fixed
// to 1d/BTC and keeps its original columns).
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
);

} // namespace research_studies
