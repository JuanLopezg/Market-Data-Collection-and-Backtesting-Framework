#include "backtest.h"

#include <cassert>
#include <cmath>

#include "logger.h"


Backtester::Backtester(BacktestContext& backtestContext)
    : backtest_context_(backtestContext),
      simulated_exchange_(backtestContext.GetCommissionMarketRate()),
      trading_engine_(
          backtestContext.GetStrategyPortfolio(),
          backtestContext.GetAccount(),
          backtestContext.GetTradeRecorder(),
          backtestContext.GetIndicatorEngine(),
          simulated_exchange_
      )
{}


PriceSnapshot Backtester::buildOpenPrices(const CoinBarMap& bars)
{
    PriceSnapshot prices;

    for (const auto& [coin, bar] : bars) {
        if (std::isfinite(bar.open) && bar.open > 0.0)
            prices.set(coin, bar.open);
    }

    return prices;
}


PriceSnapshot Backtester::buildClosePrices(const CoinBarMap& bars)
{
    PriceSnapshot prices;

    for (const auto& [coin, bar] : bars) {
        if (std::isfinite(bar.close) && bar.close > 0.0)
            prices.set(coin, bar.close);
    }

    return prices;
}


/**************************************************************************************
 * Purpose : Execute plans from the previous close at this historical bar open
 **************************************************************************************/
void Backtester::executePendingPlansAtOpen(Timestamp ts, const CoinBarMap& bars)
{
    // Previously submitted orders are allowed to fill before considering a new target.
    simulated_exchange_.processOpen(ts, bars);
    trading_engine_.processExchangeEvents();

    if (!trading_engine_.hasPendingPlans())
        return;

    const ExecutionReferencePrices prices = buildOpenPrices(bars);
    trading_engine_.executePendingPlans(ts, prices);

    // SimulatedExchange acknowledges synchronously; live adapters may acknowledge later.
    trading_engine_.processExchangeEvents();

    simulated_exchange_.processOpen(ts, bars);
    trading_engine_.processExchangeEvents();

    backtest_context_.setLastGlobalTarget(trading_engine_.lastGlobalTarget());
}


/**************************************************************************************
 * Purpose : Let shared TradingEngine process a completed historical bar close
 **************************************************************************************/
void Backtester::calculateClosePlans(
    const MarketData& marketData,
    Timestamp ts,
    const PriceSnapshot& closePrices
)
{
    trading_engine_.onBarClose(marketData, ts, closePrices);

    // Volatility diagnostics remain backtest analytics rather than TradingEngine state.
    for (const auto& strategy : backtest_context_.GetStrategyPortfolio()) {
        if (strategy.sizingDiagnostics()) {
            backtest_context_.recordVolatilityDiagnostic(
                ts,
                strategy.id(),
                strategy.name(),
                *strategy.sizingDiagnostics()
            );
        }
    }
}


/**************************************************************************************
 * Purpose : Historical clock. Strategy/execution orchestration lives in TradingEngine.
 **************************************************************************************/
void Backtester::loop()
{
    LG_INFO("Starting backtest");

    const MarketData& marketData = backtest_context_.GetMarketData();
    assert(!marketData.empty());

    starting_date_ = marketData.begin()->first;

    const std::size_t totalSteps = marketData.size();
    std::size_t currentStep = 0;
    int lastLoggedPercent = 0;
    constexpr int LOG_STEP = 5;

    for (auto it = marketData.begin(); it != marketData.end(); ++it) {
        ++currentStep;

        const int percent = static_cast<int>(
            (static_cast<double>(currentStep) /
             static_cast<double>(totalSteps)) * 100.0
        );

        if (percent >= lastLoggedPercent + LOG_STEP) {
            LG_INFO("Backtest progress: {}%", percent);
            lastLoggedPercent = percent;
        }

        const Timestamp ts = it->first;
        const CoinBarMap& bars = it->second;

        // Plans built from the previous completed close become executable now.
        executePendingPlansAtOpen(ts, bars);

        // Mark account at this completed close before sizing today's strategy signals.
        const PriceSnapshot closePrices = buildClosePrices(bars);
        backtest_context_.recordAccountSnapshot(ts, closePrices);

        calculateClosePlans(marketData, ts, closePrices);
    }

    LG_INFO("Backtest finished");
}
