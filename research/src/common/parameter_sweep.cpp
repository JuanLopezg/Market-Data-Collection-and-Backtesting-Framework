#include "parameter_sweep.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace research_studies {

std::vector<double> makeSweepValues(const SensitivityParameterDefinition& parameter)
{
    std::vector<double> values;

    if (!parameter.enabled ||
        !std::isfinite(parameter.minimum) ||
        !std::isfinite(parameter.maximum) ||
        !std::isfinite(parameter.spacing) ||
        parameter.spacing <= 0.0 ||
        parameter.maximum < parameter.minimum) {
        return values;
    }

    constexpr std::size_t safetyLimit = 100000U;
    constexpr double epsilon = 1e-10;

    for (std::size_t index = 0U; index < safetyLimit; ++index) {
        const double value = parameter.minimum +
            static_cast<double>(index) * parameter.spacing;

        if (value > parameter.maximum + epsilon) {
            break;
        }

        values.push_back(value);
    }

    return values;
}

std::vector<ActiveSensitivityParameter> activeSensitivityParameters(
    const StrategyDefinition& definition,
    bool keepEndpointsOnly
)
{
    std::vector<ActiveSensitivityParameter> active;

    for (const SensitivityParameterDefinition& parameter :
         definition.sensitivityParameters) {
        if (!parameter.enabled) {
            continue;
        }

        std::vector<double> values = makeSweepValues(parameter);
        if (values.empty()) {
            LG_WARN(
                "Skipping invalid sensitivity parameter '{}.{}'",
                definition.name,
                parameter.key
            );
            continue;
        }

        if (keepEndpointsOnly && values.size() > 2U) {
            values = {values.front(), values.back()};
        }
        active.push_back(ActiveSensitivityParameter{&parameter, std::move(values)});
    }

    return active;
}

std::size_t countSensitivityCombinations(
    const StrategyDefinition& definition,
    bool keepEndpointsOnly
)
{
    const std::vector<ActiveSensitivityParameter> active =
        activeSensitivityParameters(definition, keepEndpointsOnly);

    if (active.empty()) {
        return 0U;
    }

    ParameterValues parameters = definition.currentParameters;
    std::size_t count = 0U;
    auto counter = [&count](const ParameterValues&) { ++count; };
    enumerateSensitivityCombinations(active, 0U, parameters, counter);
    return count;
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;

    if (values.size() % 2U == 0U) {
        return (values[middle - 1U] + values[middle]) / 2.0;
    }

    return values[middle];
}

SensitivityAggregator::SensitivityAggregator(
    const std::vector<ActiveSensitivityParameter>& activeParameters
)
    : active_(activeParameters)
{
    accumulators_.reserve(active_.size());
    valueIndexes_.reserve(active_.size());
    for (const ActiveSensitivityParameter& parameter : active_) {
        accumulators_.emplace_back(parameter.values.size());

        std::map<double, std::size_t> indexes;
        for (std::size_t index = 0U; index < parameter.values.size(); ++index) {
            indexes.insert_or_assign(parameter.values[index], index);
        }
        valueIndexes_.push_back(std::move(indexes));
    }
}

void SensitivityAggregator::add(const ParameterValues& combination, const BacktestMetrics& metrics)
{
    for (std::size_t parameterIndex = 0U; parameterIndex < active_.size(); ++parameterIndex) {
        const double value = parameterOr(combination, active_[parameterIndex].definition->key, 0.0);
        const auto valueIndex = valueIndexes_[parameterIndex].find(value);
        if (valueIndex == valueIndexes_[parameterIndex].end()) {
            continue;
        }

        Accumulator& accumulator = accumulators_[parameterIndex][valueIndex->second];
        accumulator.finalReturnPercentValues.push_back(metrics.netReturnPercent);
        accumulator.maxDrawdownPercentValues.push_back(metrics.maxDrawdownPercent);
        accumulator.tradeCountValues.push_back(static_cast<double>(metrics.tradeCount));
        if (metrics.netReturnPercent > 0.0) {
            ++accumulator.positiveCombinationCount;
        }
        if (metrics.tradeCount == 0U) {
            ++accumulator.zeroTradeCombinationCount;
        }
    }
}

std::vector<ParameterSensitivityReport> SensitivityAggregator::reports() const
{
    std::vector<ParameterSensitivityReport> reports;
    reports.reserve(active_.size());

    for (std::size_t parameterIndex = 0U; parameterIndex < active_.size(); ++parameterIndex) {
        const ActiveSensitivityParameter& parameter = active_[parameterIndex];
        ParameterSensitivityReport report;
        report.parameterName = parameter.definition->key;
        report.displayName = parameter.definition->displayName;
        report.points.reserve(parameter.values.size());

        for (std::size_t valueIndex = 0U; valueIndex < parameter.values.size(); ++valueIndex) {
            const Accumulator& accumulator = accumulators_[parameterIndex][valueIndex];
            report.points.push_back(ParameterSensitivityPoint{
                parameter.values[valueIndex],
                median(accumulator.finalReturnPercentValues),
                median(accumulator.maxDrawdownPercentValues),
                median(accumulator.tradeCountValues),
                accumulator.positiveCombinationCount,
                accumulator.finalReturnPercentValues.size(),
                accumulator.zeroTradeCombinationCount
            });
        }

        reports.push_back(std::move(report));
    }

    return reports;
}

SensitivityProgressReporter::SensitivityProgressReporter(std::size_t totalRuns)
    : totalRuns_(totalRuns), start_(std::chrono::steady_clock::now())
{
    if (totalRuns_ > 0U) {
        std::cout << "\nParameter sensitivity: " << totalRuns_
                  << " isolated backtests planned.\n";
        render();
    }
}

void SensitivityProgressReporter::setStrategy(const std::string& strategyName)
{
    strategyName_ = strategyName;
    render();
}

void SensitivityProgressReporter::advance()
{
    if (completedRuns_ < totalRuns_) {
        ++completedRuns_;
    }

    const std::size_t renderInterval = std::max<std::size_t>(1U, totalRuns_ / 1000U);
    if (completedRuns_ == totalRuns_ || completedRuns_ % renderInterval == 0U) {
        render();
    }
}

void SensitivityProgressReporter::finish()
{
    if (totalRuns_ > 0U) {
        render();
        std::cout << '\n';
    }
}

std::string SensitivityProgressReporter::formatDuration(std::chrono::seconds duration)
{
    const auto totalSeconds = duration.count();
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;

    std::ostringstream output;
    if (hours > 0) {
        output << hours << "h ";
    }
    if (hours > 0 || minutes > 0) {
        output << minutes << "m ";
    }
    output << seconds << "s";
    return output.str();
}

void SensitivityProgressReporter::render()
{
    if (totalRuns_ == 0U) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_);

    const double ratio = static_cast<double>(completedRuns_) /
        static_cast<double>(totalRuns_);
    constexpr int barWidth = 34;
    const int filled = static_cast<int>(std::round(ratio * barWidth));

    std::chrono::seconds remaining(0);
    if (completedRuns_ > 0U && completedRuns_ < totalRuns_) {
        const double remainingSeconds =
            static_cast<double>(elapsed.count()) *
            static_cast<double>(totalRuns_ - completedRuns_) /
            static_cast<double>(completedRuns_);
        remaining = std::chrono::seconds(
            static_cast<long long>(std::max(0.0, remainingSeconds))
        );
    }

    std::cout << '\r' << '[';
    for (int index = 0; index < barWidth; ++index) {
        std::cout << (index < filled ? '=' : ' ');
    }
    std::cout << "] " << std::fixed << std::setprecision(1)
              << (ratio * 100.0) << "% "
              << completedRuns_ << '/' << totalRuns_
              << " | " << strategyName_
              << " | elapsed " << formatDuration(elapsed)
              << " | ETA " << formatDuration(remaining)
              << std::flush;
}

} // namespace research_studies
