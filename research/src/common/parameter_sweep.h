#pragma once

#include "backtest_html_report.h"
#include "backtest_metrics.h"
#include "research_strategy_definitions.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Parameter-sensitivity sweep shared by the single-strategy study executables:
// grid values per parameter, the cartesian walk over them, the per-value median
// aggregation shown in the HTML report, and the terminal progress bar. Running
// each grid point and exporting it stays with the caller.
namespace research_studies {

// One enabled parameter axis with the values the sweep visits.
struct ActiveSensitivityParameter {
    const SensitivityParameterDefinition* definition = nullptr;
    std::vector<double> values;
};

// minimum, minimum + spacing, ... up to maximum (1e-10 tolerance). Empty for a
// disabled or malformed axis.
std::vector<double> makeSweepValues(const SensitivityParameterDefinition& parameter);

// Enabled axes with at least one value. keepEndpointsOnly reduces every axis to its
// first and last value (the compact grid of the BTC and initial-parameter studies).
std::vector<ActiveSensitivityParameter> activeSensitivityParameters(
    const StrategyDefinition& definition,
    bool keepEndpointsOnly = false
);

// Calls callback(parameters) once per grid point. Axes vary in definition order;
// parameters starts as the strategy's current values, so inactive keys keep them.
template <typename Callback>
void enumerateSensitivityCombinations(
    const std::vector<ActiveSensitivityParameter>& activeParameters,
    std::size_t parameterIndex,
    ParameterValues& parameters,
    Callback& callback
)
{
    if (parameterIndex >= activeParameters.size()) {
        callback(parameters);
        return;
    }

    const ActiveSensitivityParameter& active = activeParameters[parameterIndex];
    for (const double value : active.values) {
        parameters.insert_or_assign(active.definition->key, value);
        enumerateSensitivityCombinations(activeParameters, parameterIndex + 1U, parameters, callback);
    }
}

// Every grid point, valid or not. Zero when no axis is active.
std::size_t countSensitivityCombinations(
    const StrategyDefinition& definition,
    bool keepEndpointsOnly = false
);

double median(std::vector<double> values);

// Collects successful runs per axis value and reports, for every value, the
// medians across all runs that share it.
class SensitivityAggregator {
public:
    explicit SensitivityAggregator(const std::vector<ActiveSensitivityParameter>& activeParameters);

    void add(const ParameterValues& combination, const BacktestMetrics& metrics);
    std::vector<ParameterSensitivityReport> reports() const;

private:
    struct Accumulator {
        std::vector<double> finalReturnPercentValues;
        std::vector<double> maxDrawdownPercentValues;
        std::vector<double> tradeCountValues;
        std::size_t positiveCombinationCount = 0U;
        std::size_t zeroTradeCombinationCount = 0U;
    };

    const std::vector<ActiveSensitivityParameter>& active_;
    std::vector<std::vector<Accumulator>> accumulators_;
    std::vector<std::map<double, std::size_t>> valueIndexes_;
};

// Single-line terminal progress bar with elapsed time and ETA for a whole study.
class SensitivityProgressReporter {
public:
    explicit SensitivityProgressReporter(std::size_t totalRuns);

    void setStrategy(const std::string& strategyName);
    void advance();
    void finish();

private:
    static std::string formatDuration(std::chrono::seconds duration);
    void render();

    std::size_t totalRuns_ = 0U;
    std::size_t completedRuns_ = 0U;
    std::string strategyName_ = "Preparing";
    std::chrono::steady_clock::time_point start_;
};

} // namespace research_studies
