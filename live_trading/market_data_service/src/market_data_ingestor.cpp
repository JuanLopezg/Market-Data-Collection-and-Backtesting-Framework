/*
 * File purpose: Runs one complete daily ingestion cycle: universe selection, downloads, validation, and storage commit.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#include "market_data_ingestor.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "service_logging.h"
#include "time_utils.h"

namespace {
constexpr auto EMPTY_DATE = std::chrono::year{2000}/1/1;
}

MarketDataIngestor::MarketDataIngestor(MarketDataConfig config)
    : config_(std::move(config)),
      binance_(config_.binance_base_url)
{}

TrackedMarketData MarketDataIngestor::updateTracker(
    const TrackedMarketData& previous,
    const std::vector<std::pair<std::string, double>>& ranked,
    std::chrono::year_month_day targetDate) const
{
    if (ranked.size() < config_.active_top_n)
        throw std::runtime_error("Binance ranking contains fewer symbols than active_top_n");

    std::set<std::string> currentTop;
    for (std::size_t i = 0; i < config_.active_top_n; ++i)
        currentTop.insert(ranked[i].first);

    TrackedMarketData result;
    result.date = targetDate;

    const bool hasPrevious = previous.date != EMPTY_DATE && !previous.days_since_top_n.empty();
    int dayDifference = 1;
    if (hasPrevious) {
        dayDifference = static_cast<int>(
            (std::chrono::sys_days{targetDate} - std::chrono::sys_days{previous.date}).count());
        if (dayDifference < 0)
            throw std::runtime_error("Refusing to move tracked-pair state backwards in time");
        if (dayDifference == 0)
            dayDifference = 0;
    }

    for (const auto& symbol : currentTop)
        result.days_since_top_n[symbol] = 0;

    if (hasPrevious) {
        for (const auto& [symbol, oldDays] : previous.days_since_top_n) {
            if (currentTop.contains(symbol))
                continue;
            result.days_since_top_n[symbol] = oldDays + dayDifference;
        }
    }

    return result;
}

MarketDataIngestionSummary MarketDataIngestor::run(std::chrono::year_month_day targetDate)
{
    LG_INFO(
        "service=market-data event=daily_ingestion_started target_date={} database={}",
        formatYMD(targetDate), config_.database_path.string()
    );

    MarketDataStore store(config_.database_path);
    const TrackedMarketData previous = store.loadTracked();

    if (previous.date != EMPTY_DATE && std::chrono::sys_days{previous.date} > std::chrono::sys_days{targetDate})
        throw std::runtime_error("Market-data database is ahead of the requested live target date");

    const BinanceUniverseSnapshot universe = binance_.universeByQuoteVolume(config_.ranking_size);
    const auto& ranked = universe.ranked_top;
    if (ranked.size() < config_.active_top_n)
        throw std::runtime_error("Unable to obtain enough ranked Binance USDT perpetuals");

    const TrackedMarketData tracked = updateTracker(previous, ranked, targetDate);

    auto plan = store.buildDownloadPlan(
        tracked,
        targetDate,
        config_.retain_after_top_n_days,
        config_.minimum_history_days
    );

    // A symbol can remain in our 365-day tracker after it stops being tradable.
    // Keep its historical rows and tracker state, but do not ask Binance for impossible
    // new candles after exchangeInfo no longer reports it as an active USDT perpetual.
    plan.erase(
        std::remove_if(plan.begin(), plan.end(), [&](const MarketDataDownloadRequest& request) {
            if (universe.eligible_symbols.contains(request.symbol))
                return false;
            LG_WARN(
                "service=market-data event=tracked_symbol_not_currently_eligible symbol={} action=skip_download",
                request.symbol
            );
            return true;
        }),
        plan.end()
    );

    LG_INFO(
        "service=market-data event=download_plan target_date={} ranked={} active_top_n={} tracked={} requests={}",
        formatYMD(targetDate), ranked.size(), config_.active_top_n,
        tracked.days_since_top_n.size(), plan.size()
    );

    const BinanceFetchResult fetched = binance_.fetchDailyBars(plan, config_.max_parallel_requests);
    if (!fetched.failed_symbols.empty()) {
        std::string names;
        for (const auto& symbol : fetched.failed_symbols) {
            if (!names.empty())
                names += ',';
            names += symbol;
        }
        throw std::runtime_error("Binance OHLCV request failed for: " + names);
    }

    std::size_t rows = 0;
    for (const auto& [_, byDate] : fetched.bars.data)
        rows += byDate.size();

    // Important durability boundary: tracker + ranking + OHLCV are committed together.
    // The caller publishes MARKET_DATA_UPDATED only AFTER this commit returns.
    store.commitDailyUpdate(tracked, fetched.bars, ranked, targetDate);

    std::size_t maintained = 0;
    for (const auto& [_, days] : tracked.days_since_top_n)
        if (days <= config_.retain_after_top_n_days)
            ++maintained;

    MarketDataIngestionSummary summary;
    summary.target_date = targetDate;
    summary.ranked_symbols = ranked.size();
    summary.active_top_n = config_.active_top_n;
    summary.tracked_symbols = tracked.days_since_top_n.size();
    summary.maintained_symbols = maintained;
    summary.requested_symbols = plan.size();
    summary.downloaded_rows = rows;

    LG_INFO(
        "service=market-data event=daily_ingestion_committed target_date={} ranked={} top_n={} tracked={} maintained={} requested={} downloaded_rows={}",
        formatYMD(targetDate), summary.ranked_symbols, summary.active_top_n,
        summary.tracked_symbols, summary.maintained_symbols,
        summary.requested_symbols, summary.downloaded_rows
    );

    return summary;
}
