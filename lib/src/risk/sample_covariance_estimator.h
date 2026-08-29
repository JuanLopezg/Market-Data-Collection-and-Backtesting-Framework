#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "covariance_estimator.h"


/**************************************************************************************
 * Type    : SampleCovarianceEstimator
 * Purpose : Estimates annualized covariance from rolling close-to-close simple returns
 *
 * The estimator uses the most recent lookbackReturns valid return observations ending
 * at or before timestamp. A return observation is used only when every requested coin
 * has a finite, positive close on both consecutive market timestamps.
 *
 * periodsPerYear annualizes the per-bar covariance. For daily crypto data this will
 * normally be 365. The value is explicit so the risk code is not tied to one timeframe.
 **************************************************************************************/
class SampleCovarianceEstimator final : public CovarianceEstimator {
private:
    std::size_t lookbackReturns_ = 0;
    double periodsPerYear_ = 0.0;

public:
    SampleCovarianceEstimator(std::size_t lookbackReturns, double periodsPerYear)
        : lookbackReturns_(lookbackReturns), periodsPerYear_(periodsPerYear)
    {
        if (lookbackReturns_ < 2)
            throw std::invalid_argument("Covariance lookback must contain at least 2 returns");

        if (!std::isfinite(periodsPerYear_) || periodsPerYear_ <= 0.0)
            throw std::invalid_argument("Periods per year must be finite and positive");
    }

    std::optional<CovarianceMatrix> estimate(
        const MarketData& marketData,
        Timestamp timestamp,
        const std::vector<Coin>& coins
    ) const override
    {
        if (coins.empty())
            return CovarianceMatrix();

        std::vector<Coin> orderedCoins = coins;
        std::sort(orderedCoins.begin(), orderedCoins.end());

        if (std::adjacent_find(orderedCoins.begin(), orderedCoins.end()) != orderedCoins.end())
            throw std::invalid_argument("Covariance request contains duplicate coins");

        auto currentIt = marketData.upper_bound(timestamp);
        if (currentIt == marketData.begin())
            return std::nullopt;

        --currentIt;

        std::vector<std::vector<double>> returns;
        returns.reserve(lookbackReturns_);

        while (currentIt != marketData.begin() && returns.size() < lookbackReturns_) {
            auto previousIt = currentIt;
            --previousIt;

            std::vector<double> observation;
            observation.reserve(orderedCoins.size());
            bool valid = true;

            for (const Coin& coin : orderedCoins) {
                const auto currentBar = currentIt->second.find(coin);
                const auto previousBar = previousIt->second.find(coin);

                if (currentBar == currentIt->second.end() || previousBar == previousIt->second.end()) {
                    valid = false;
                    break;
                }

                const double currentClose = currentBar->second.close;
                const double previousClose = previousBar->second.close;

                if (!std::isfinite(currentClose) || !std::isfinite(previousClose) ||
                    currentClose <= 0.0 || previousClose <= 0.0) {
                    valid = false;
                    break;
                }

                observation.push_back(currentClose / previousClose - 1.0);
            }

            if (valid)
                returns.push_back(std::move(observation));

            currentIt = previousIt;
        }

        if (returns.size() < lookbackReturns_)
            return std::nullopt;

        const std::size_t assetCount = orderedCoins.size();
        const double sampleCount = static_cast<double>(returns.size());
        std::vector<double> means(assetCount, 0.0);

        for (const auto& observation : returns) {
            for (std::size_t i = 0; i < assetCount; ++i)
                means[i] += observation[i];
        }

        for (double& mean : means)
            mean /= sampleCount;

        std::vector<std::vector<double>> covariance(
            assetCount,
            std::vector<double>(assetCount, 0.0)
        );

        for (std::size_t i = 0; i < assetCount; ++i) {
            for (std::size_t j = i; j < assetCount; ++j) {
                double sum = 0.0;

                for (const auto& observation : returns)
                    sum += (observation[i] - means[i]) * (observation[j] - means[j]);

                const double value = (sum / (sampleCount - 1.0)) * periodsPerYear_;
                covariance[i][j] = value;
                covariance[j][i] = value;
            }
        }

        return CovarianceMatrix(std::move(orderedCoins), std::move(covariance));
    }

    std::size_t lookbackReturns() const
    {
        return lookbackReturns_;
    }

    double periodsPerYear() const
    {
        return periodsPerYear_;
    }
};
