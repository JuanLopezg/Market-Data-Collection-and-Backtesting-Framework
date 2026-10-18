#include "chain_profile.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "relative_threshold_rebalance_policy.h"
#include "sample_covariance_estimator.h"
#include "threshold_rebalance_policy.h"
#include "validated/pure_rsi.h"
#include "volatility_target_sizer.h"

namespace chain_profile {

using json = nlohmann::json;

namespace {

std::unique_ptr<PortfolioSizer> makeSizer(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "equal_weight")
        return std::make_unique<EqualWeightSizer>(value.at("weight_per_full_signal").get<double>());
    if (type == "volatility_target")
        return std::make_unique<VolatilityTargetSizer>(
            std::make_unique<SampleCovarianceEstimator>(
                value.at("covariance_lookback").get<std::size_t>(),
                value.at("periods_per_year").get<double>()),
            value.at("target_volatility").get<double>());
    throw std::runtime_error("Unsupported chain profile sizer: " + type);
}

std::unique_ptr<RebalancePolicy> makeRebalancePolicy(const json& value)
{
    const std::string type = value.at("type").get<std::string>();
    if (type == "entry_exit_only")
        return std::make_unique<EntryExitOnlyRebalancePolicy>();
    if (type == "threshold")
        return std::make_unique<ThresholdRebalancePolicy>(value.at("threshold").get<double>());
    if (type == "relative_threshold")
        return std::make_unique<RelativeThresholdRebalancePolicy>(value.at("threshold").get<double>());
    throw std::runtime_error("Unsupported chain profile rebalance policy: " + type);
}

// Indicator specs as the strategy service parses them (parseIndicatorSpec).
IndicatorKind parseIndicatorKind(const std::string& value)
{
    if (value == "SMA") return IndicatorKind::SMA;
    if (value == "EMA") return IndicatorKind::EMA;
    if (value == "RSI") return IndicatorKind::RSI;
    if (value == "ATR") return IndicatorKind::ATR;
    if (value == "ROC") return IndicatorKind::ROC;
    if (value == "Highest") return IndicatorKind::Highest;
    if (value == "Lowest") return IndicatorKind::Lowest;
    if (value == "DonchianHigh") return IndicatorKind::DonchianHigh;
    if (value == "DonchianLow") return IndicatorKind::DonchianLow;
    if (value == "DonchianMid") return IndicatorKind::DonchianMid;
    throw std::invalid_argument("Unsupported indicator kind: " + value);
}

PriceField parsePriceField(const std::string& value)
{
    if (value == "Open") return PriceField::Open;
    if (value == "High") return PriceField::High;
    if (value == "Low") return PriceField::Low;
    if (value == "Close") return PriceField::Close;
    if (value == "Volume") return PriceField::Volume;
    if (value == "QuoteVolume") return PriceField::QuoteVolume;
    throw std::invalid_argument("Unsupported indicator source: " + value);
}

IndicatorSpec parseIndicatorSpec(const json& value)
{
    IndicatorSpec spec{
        parseIndicatorKind(value.at("kind").get<std::string>()),
        parsePriceField(value.at("source").get<std::string>()),
        value.at("length").get<unsigned int>(),
        value.value("offset", 0u)
    };
    if (spec.length == 0)
        throw std::invalid_argument("Indicator length must be positive");
    return spec;
}

} // namespace

json readJson(const std::string& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open " + path);
    return json::parse(input);
}

StrategyPortfolio makeStrategyPortfolio(const json& strategy, const json& portfolio)
{
    if (strategy.at("type") != "PureRSI" ||
        strategy.at("universe").at("indicator").at("source") != "QuoteVolume")
        throw std::runtime_error("Unsupported chain profile: only PureRSI over the QuoteVolume universe");

    const auto& parameters = strategy.at("parameters");
    const unsigned rsiLength = parameters.at("rsi_length");
    StrategyPortfolio strategies;
    strategies.emplace_back(
        strategy.at("id").get<unsigned>(),
        std::make_unique<StrategyPureRSI>(
            strategy.at("max_active_signals"),
            std::make_unique<TopNLiquidityUniverse>(
                IndicatorSpec{IndicatorKind::SMA, PriceField::QuoteVolume, 25},
                strategy.at("universe").at("count"), true),
            std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::RSI, PriceField::Close, rsiLength}, true),
            strategy.at("max_ranking_position"), rsiLength,
            parameters.at("rsi_entry"), parameters.at("rsi_exit")),
        portfolio.at("allocation_weight"),
        makeSizer(portfolio.at("sizer")),
        RiskConstraints(portfolio.at("risk").at("max_gross_leverage"),
                        portfolio.at("risk").at("max_asset_weight")),
        makeRebalancePolicy(portfolio.at("rebalance")));
    return strategies;
}

StrategySignalPortfolio makeSignalPortfolio(const json& strategyFile)
{
    StrategySignalPortfolio strategies;
    for (const json& value : strategyFile.at("strategies")) {
        if (value.at("type") != "PureRSI")
            throw std::invalid_argument("Unsupported strategy type: " + value.at("type").get<std::string>());
        const json& universe = value.at("universe");
        const json& ranker = value.at("ranker");
        if (universe.at("type") != "top_n_liquidity" || ranker.at("type") != "indicator")
            throw std::invalid_argument("Unsupported universe or ranker type");
        const json& parameters = value.at("parameters");
        strategies.emplace_back(
            value.at("id").get<StrategyID>(),
            std::make_unique<StrategyPureRSI>(
                value.at("max_active_signals").get<unsigned int>(),
                std::make_unique<TopNLiquidityUniverse>(
                    parseIndicatorSpec(universe.at("indicator")),
                    universe.at("count").get<unsigned int>(),
                    universe.value("descending", true), true),
                std::make_unique<IndicatorRanker>(
                    parseIndicatorSpec(ranker.at("indicator")), ranker.value("descending", true), true),
                value.at("max_ranking_position").get<unsigned int>(),
                parameters.at("rsi_length").get<unsigned int>(),
                parameters.at("rsi_entry").get<double>(),
                parameters.at("rsi_exit").get<double>()));
    }
    if (strategies.empty())
        throw std::invalid_argument("Strategy config must contain at least one strategy");
    return strategies;
}

std::vector<PortfolioRiskStrategyConfig> makeRiskConfigs(const json& portfolioFile)
{
    std::vector<PortfolioRiskStrategyConfig> strategies;
    for (const json& value : portfolioFile.at("strategies")) {
        const json& risk = value.at("risk");
        strategies.emplace_back(
            value.at("id").get<StrategyID>(),
            value.at("name").get<std::string>(),
            value.at("allocation_weight").get<double>(),
            makeSizer(value.at("sizer")),
            RiskConstraints(risk.at("max_gross_leverage").get<double>(), risk.at("max_asset_weight").get<double>()),
            makeRebalancePolicy(value.at("rebalance")));
    }
    if (strategies.empty())
        throw std::invalid_argument("Portfolio config must contain at least one strategy");
    return strategies;
}

unsigned int riskHistoryDays(const json& portfolioFile, unsigned int bufferDays)
{
    std::size_t maxLookbackReturns = 0;
    for (const json& value : portfolioFile.at("strategies")) {
        const json& sizer = value.at("sizer");
        if (sizer.at("type").get<std::string>() == "volatility_target")
            maxLookbackReturns = std::max(maxLookbackReturns, sizer.at("covariance_lookback").get<std::size_t>());
    }
    if (maxLookbackReturns == 0)
        return 1; // Equal weight only needs today's close for equity/rebalance pricing.
    return static_cast<unsigned int>(maxLookbackReturns + 1U + bufferDays);
}

std::size_t sizerHistoryDays(const json& sizer)
{
    if (sizer.at("type").get<std::string>() != "volatility_target")
        return 0;
    return sizer.at("covariance_lookback").get<std::size_t>() + 1U + 5U;
}

void addSizerHistory(MarketData& marketData, const OHLCVData& raw, Timestamp closeDate, std::size_t historyDays)
{
    std::set<Timestamp> dates;
    for (const auto& [coin, bars] : raw.data)
        for (const auto& [date, bar] : bars)
            if (date < closeDate)
                dates.insert(date);
    std::vector<Timestamp> recent(dates.begin(), dates.end());
    const std::size_t keep = historyDays > 0 ? historyDays - 1U : 0U;
    if (recent.size() > keep)
        recent.erase(recent.begin(), recent.end() - static_cast<std::ptrdiff_t>(keep));
    for (const Timestamp date : recent) {
        CoinBarMap bars;
        for (const auto& [coin, history] : raw.data) {
            const auto found = history.find(date);
            if (found == history.end())
                continue;
            BarData bar; // Only the closes matter for the covariance estimate.
            bar.open = found->second.open;
            bar.high = found->second.high;
            bar.low = found->second.low;
            bar.close = found->second.close;
            bar.volume = found->second.volume;
            bar.quote_volume = found->second.quote_volume;
            bars.emplace(coin, bar);
        }
        marketData.emplace(date, std::move(bars));
    }
}

} // namespace chain_profile
