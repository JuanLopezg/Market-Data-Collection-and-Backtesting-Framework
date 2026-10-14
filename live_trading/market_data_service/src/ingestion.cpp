// Runs one complete daily ingestion cycle: universe selection, downloads, validation, and
// storage commit.

#include "ingestion.h"

#include <algorithm>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>

#include "service_logging.h"
#include "time_utils.h"

namespace {
constexpr auto EMPTY_DATE = std::chrono::year{2000}/1/1;
// How many times the ranking is re-read while Binance's clock is still before the day end.
constexpr int MAX_SERVER_CLOCK_WAITS = 30;
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

BinanceUniverseSnapshot MarketDataIngestor::universeAfterDayClose(std::chrono::year_month_day targetDate) const
{
    const long long dayEndMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::sys_days{targetDate} + std::chrono::days{1}).time_since_epoch()).count();
    for (int attempt = 1; attempt <= MAX_SERVER_CLOCK_WAITS; ++attempt) {
        BinanceUniverseSnapshot universe = binance_.universeByQuoteVolume(config_.ranking_size);
        if (universe.ranked_top.empty())
            return universe; // The caller reports the failed ranking.
        if (universe.server_time_ms == 0)
            throw std::runtime_error("Binance exchangeInfo has no serverTime; cannot prove the day is closed");
        if (universe.server_time_ms >= dayEndMs)
            return universe;
        const long long behindMs = dayEndMs - universe.server_time_ms;
        LG_WARN("Binance clock is {} ms before the end of {}; waiting for its candles to close",
                behindMs, formatYMD(targetDate));
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(behindMs, 1000) + 50));
    }
    throw std::runtime_error("Binance clock did not reach the end of " + formatYMD(targetDate));
}

MarketDataIngestionSummary MarketDataIngestor::run(std::chrono::year_month_day targetDate)
{
    LG_INFO("Starting daily download for {}", formatYMD(targetDate));
    LG_DEBUG("Market database {}", config_.database_path.string());

    MarketDataStore store(config_.database_path);
    const TrackedMarketData previous = store.loadTracked();

    if (previous.date != EMPTY_DATE && std::chrono::sys_days{previous.date} > std::chrono::sys_days{targetDate})
        throw std::runtime_error("Market-data database is ahead of the requested live target date");

    const BinanceUniverseSnapshot universe = universeAfterDayClose(targetDate);
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
            LG_WARN("{} is tracked but not currently tradable; skipping its download", request.symbol);
            return true;
        }),
        plan.end()
    );

    LG_INFO("Download plan for {}: {} ranked symbols, top {}, {} tracked, {} requests",
            formatYMD(targetDate), ranked.size(), config_.active_top_n, tracked.days_since_top_n.size(), plan.size());

    // Opening prices of the next day are downloaded at the same time as the completed candles.
    std::future<std::unordered_map<Coin, double>> paperPricesDownload;
    if (config_.publish_paper_execution_prices) {
        std::vector<std::string> symbols;
        for (const auto& [symbol, days] : tracked.days_since_top_n) {
            if (days <= config_.retain_after_top_n_days && universe.eligible_symbols.contains(symbol))
                symbols.push_back(symbol);
        }
        // Opening prices are kept outside completed-bar SQLite and cannot enter
        // strategy features. Only the simulated exchange receives open(T+1).
        const auto openDate = std::chrono::year_month_day{std::chrono::sys_days{targetDate} + std::chrono::days{1}};
        paperPricesDownload = std::async(std::launch::async, [this, openDate, symbols = std::move(symbols)] {
            return binance_.fetchOpeningPrices(openDate, symbols, config_.max_parallel_requests);
        });
    }

    const BinanceFetchResult fetched = binance_.fetchDailyBars(plan, config_.max_parallel_requests);
    std::unordered_map<Coin, double> paperPrices;
    if (paperPricesDownload.valid())
        paperPrices = paperPricesDownload.get();
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
    summary.paper_opening_prices = std::move(paperPrices);

    LG_INFO("Saved daily data for {}: {} rows downloaded for {} symbols ({} ranked, top {}, {} tracked, {} maintained)",
            formatYMD(targetDate),
            summary.downloaded_rows,
            summary.requested_symbols,
            summary.ranked_symbols,
            summary.active_top_n,
            summary.tracked_symbols,
            summary.maintained_symbols);

    return summary;
}
