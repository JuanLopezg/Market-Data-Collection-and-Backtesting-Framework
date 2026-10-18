// Runs one complete daily ingestion cycle: universe selection, downloads, validation, and
// storage commit.

#include "ingestion.h"

#include <algorithm>
#include <future>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "service_logging.h"
#include "time_utils.h"

namespace {
constexpr auto EMPTY_DATE = std::chrono::year{2000}/1/1;
// How long a target day waits for proof it closed (the next day's candle, or for the
// ranking a 24h window ending after midnight), and how often that proof is asked again.
constexpr auto NEXT_DAY_WAIT = std::chrono::seconds(30);
constexpr auto NEXT_DAY_POLL = std::chrono::milliseconds(50);
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


MarketDataIngestionSummary MarketDataIngestor::republishStoredDay(
    const MarketDataStore& store,
    const TrackedMarketData& tracked,
    std::chrono::year_month_day targetDate) const
{
    MarketDataIngestionSummary summary;
    summary.target_date = targetDate;
    summary.ranked_symbols = static_cast<std::size_t>(store.countRankedFor(targetDate));
    summary.active_top_n = config_.active_top_n;
    summary.tracked_symbols = tracked.days_since_top_n.size();
    for (const auto& [_, days] : tracked.days_since_top_n)
        if (days <= config_.retain_after_top_n_days)
            ++summary.maintained_symbols;

    // The simulated exchange fills at open(T+1). Those candles opened at midnight and no
    // longer change, and a retry after a failed publication still needs them; they are the
    // only Binance request on this path, for the symbols downloaded that day.
    if (config_.publish_paper_execution_prices) {
        const auto openDate = std::chrono::year_month_day{std::chrono::sys_days{targetDate} + std::chrono::days{1}};
        summary.paper_opening_prices = binance_.fetchOpeningPrices(
            openDate, store.symbolsWithBarOn(targetDate), config_.max_parallel_requests);
    }

    LG_INFO("Daily data for {} is already stored ({} tracked, {} maintained); republishing it without "
            "ranking or downloading candles again", formatYMD(targetDate), summary.tracked_symbols,
            summary.maintained_symbols);
    return summary;
}

MarketDataPreparation MarketDataIngestor::prepare(std::chrono::year_month_day targetDate) const
{
    MarketDataPreparation prepared;
    prepared.target_date = targetDate;
    MarketDataStore store(config_.database_path);
    prepared.previous = store.loadTracked();
    if (!prepared.previous.days_since_top_n.empty() &&
        std::chrono::sys_days{prepared.previous.date} < std::chrono::sys_days{targetDate})
        prepared.early_plan = store.buildDownloadPlan(prepared.previous, targetDate,
                                                      config_.retain_after_top_n_days, config_.minimum_history_days);
    binance_.warmUp();
    LG_INFO("Prepared the daily download for {}: {} expected requests, Binance connection open",
            formatYMD(targetDate), prepared.early_plan.size());
    return prepared;
}

MarketDataIngestionSummary MarketDataIngestor::run(std::chrono::year_month_day targetDate,
                                                   const MarketDataPreparation* prepared)
{
    LG_INFO("Starting daily download for {}", formatYMD(targetDate));
    LG_DEBUG("Market database {}", config_.database_path.string());

    const auto cycleStart = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cycleStart).count();
    };

    MarketDataStore store(config_.database_path);
    const TrackedMarketData previous = store.loadTracked();

    if (previous.date != EMPTY_DATE && std::chrono::sys_days{previous.date} > std::chrono::sys_days{targetDate})
        throw std::runtime_error("Market-data database is ahead of the requested live target date");

    // A restart during the day finds the completed day already committed. Re-ranking it now
    // would use Binance's rolling 24h volume, which includes hours after that day's close, and
    // could add symbols. Republish the stored day instead.
    if (previous.date == targetDate && !previous.days_since_top_n.empty())
        return republishStoredDay(store, previous, targetDate);

    // Every Binance request answers in about 0.25 s, so the whole cycle is one concurrent
    // burst on one HTTP/2 connection: the ranking documents and the candles of the already
    // tracked symbols (nearly always the final plan), each up to the target day plus the
    // next day's candle. That next candle proves the target day closed on Binance's own
    // clock (it starts with the first trade after midnight) and carries open(T+1), so no
    // clock request and no separate opening-price requests are needed. Only symbols the
    // ranking adds wait for it. Weight: about 100 of Binance's 2400 per minute.
    const bool usePrepared = prepared && prepared->target_date == targetDate &&
                             prepared->previous.date == previous.date &&
                             prepared->previous.days_since_top_n == previous.days_since_top_n;
    const std::vector<MarketDataDownloadRequest> earlyPlan = usePrepared
        ? prepared->early_plan
        : previous.days_since_top_n.empty()
            ? std::vector<MarketDataDownloadRequest>{}
            : store.buildDownloadPlan(previous, targetDate, config_.retain_after_top_n_days, config_.minimum_history_days);
    const long long dayEndMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::sys_days{targetDate} + std::chrono::days{1}).time_since_epoch()).count();
    const auto giveUpAt = std::chrono::steady_clock::now() + NEXT_DAY_WAIT;

    BinanceDailySnapshot first = binance_.fetchDailySnapshot(earlyPlan, true, config_.ranking_size, config_.max_parallel_requests);
    const long long firstMs = elapsedMs();
    BinanceUniverseSnapshot universe = std::move(*first.universe);
    // The ranking must cover the whole closed day: its 24h window has to end after midnight.
    // Tickers without a closeTime (not seen on Binance) fall back to Binance's clock.
    int rankingRetries = 0;
    const auto rankingCoversDay = [&] {
        if (universe.tickers_until_ms > 0)
            return universe.tickers_until_ms >= dayEndMs;
        return binance_.serverTimeMs() >= dayEndMs;
    };
    while (!universe.ranked_top.empty() && !rankingCoversDay()) {
        if (std::chrono::steady_clock::now() >= giveUpAt)
            throw std::runtime_error("Binance 24h tickers still end before the close of " + formatYMD(targetDate));
        std::this_thread::sleep_for(NEXT_DAY_POLL);
        universe = binance_.universeByQuoteVolume(config_.ranking_size);
        ++rankingRetries;
    }
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

    // Keep exactly the planned requests: an early request the final plan dropped is
    // discarded, and one that differs or failed early is asked now.
    const auto sameRequest = [](const MarketDataDownloadRequest& a, const MarketDataDownloadRequest& b) {
        return a.symbol == b.symbol && a.start_date == b.start_date && a.end_date == b.end_date;
    };
    const std::set<std::string> failedEarly(first.candles.failed_symbols.begin(), first.candles.failed_symbols.end());
    BinanceFetchResult fetched;
    std::vector<MarketDataDownloadRequest> remaining;
    for (const auto& request : plan) {
        const bool early = !failedEarly.contains(request.symbol) &&
            std::any_of(earlyPlan.begin(), earlyPlan.end(),
                        [&](const MarketDataDownloadRequest& other) { return sameRequest(request, other); });
        if (!early) {
            remaining.push_back(request);
            continue;
        }
        if (const auto bars = first.candles.bars.data.find(request.symbol); bars != first.candles.bars.data.end())
            fetched.bars.data[request.symbol] = bars->second;
        if (const auto open = first.candles.next_opens.find(request.symbol); open != first.candles.next_opens.end())
            fetched.next_opens[request.symbol] = open->second;
    }
    const auto merge = [&](BinanceFetchResult&& more) {
        fetched.failed_symbols.insert(fetched.failed_symbols.end(), more.failed_symbols.begin(), more.failed_symbols.end());
        for (auto& [symbol, bars] : more.bars.data)
            for (auto& [date, bar] : bars)
                fetched.bars.data[symbol][date] = bar; // a later answer replaces an unfinished day
        for (const auto& [symbol, open] : more.next_opens)
            fetched.next_opens[symbol] = open;
    };
    if (!remaining.empty())
        merge(binance_.fetchDailySnapshot(remaining, false, config_.ranking_size, config_.max_parallel_requests).candles);
    const long long candlesMs = elapsedMs();

    // Closure proof for every planned symbol: its next-day candle. A symbol without one yet
    // (no trade since midnight, or Binance's clock still before it) has its target day asked
    // again until the next candle appears.
    std::vector<MarketDataDownloadRequest> unproven;
    const auto collectUnproven = [&] {
        unproven.clear();
        for (const auto& request : plan)
            if (!fetched.next_opens.contains(request.symbol) &&
                std::find(fetched.failed_symbols.begin(), fetched.failed_symbols.end(), request.symbol) == fetched.failed_symbols.end())
                unproven.push_back({request.symbol, targetDate, targetDate});
    };
    collectUnproven();
    int closurePolls = 0;
    while (!unproven.empty() && std::chrono::steady_clock::now() < giveUpAt) {
        std::this_thread::sleep_for(NEXT_DAY_POLL);
        merge(binance_.fetchDailySnapshot(unproven, false, config_.ranking_size, config_.max_parallel_requests).candles);
        ++closurePolls;
        collectUnproven();
    }
    if (!unproven.empty()) {
        // Thin symbols may not trade for a while after midnight; Binance's clock then proves
        // the day closed, and their last answer holds the final candle.
        const long long serverTimeMs = binance_.serverTimeMs();
        if (serverTimeMs < dayEndMs)
            throw std::runtime_error("Binance clock does not prove the close of " + formatYMD(targetDate));
        LG_WARN("{} symbol(s) have no candle after {} yet; Binance's clock proves the day closed", unproven.size(),
                formatYMD(targetDate));
    }
    if (!fetched.failed_symbols.empty()) {
        std::sort(fetched.failed_symbols.begin(), fetched.failed_symbols.end());
        fetched.failed_symbols.erase(std::unique(fetched.failed_symbols.begin(), fetched.failed_symbols.end()),
                                     fetched.failed_symbols.end());
        std::string names;
        for (const auto& symbol : fetched.failed_symbols) {
            if (!names.empty())
                names += ',';
            names += symbol;
        }
        throw std::runtime_error("Binance OHLCV request failed for: " + names);
    }

    // Opening prices of the next day, for the simulated exchange only: kept outside
    // completed-bar SQLite, they cannot enter strategy features. They come from the
    // next-day candles above; a maintained symbol outside the plan is asked separately. A
    // planned symbol still without one already waited NEXT_DAY_WAIT: the cycle fails.
    std::unordered_map<Coin, double> paperPrices;
    if (config_.publish_paper_execution_prices) {
        std::vector<std::string> missing;
        for (const auto& [symbol, days] : tracked.days_since_top_n) {
            if (days > config_.retain_after_top_n_days || !universe.eligible_symbols.contains(symbol))
                continue;
            const bool planned = std::any_of(plan.begin(), plan.end(),
                                             [&](const MarketDataDownloadRequest& request) { return request.symbol == symbol; });
            if (const auto open = fetched.next_opens.find(symbol); open != fetched.next_opens.end())
                paperPrices.emplace(symbol, open->second);
            else if (planned)
                throw std::runtime_error("Paper execution requires the opening price of " + symbol +
                                         ", whose next-day candle has not started");
            else
                missing.push_back(symbol);
        }
        if (!missing.empty()) {
            const auto openDate = std::chrono::year_month_day{std::chrono::sys_days{targetDate} + std::chrono::days{1}};
            const auto asked = binance_.fetchOpeningPrices(openDate, missing, config_.max_parallel_requests);
            paperPrices.insert(asked.begin(), asked.end());
        }
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
    LG_INFO("Daily download for {} took {} ms: first burst (ranking + {} candles) at {} ms, ranking asked again {} "
            "time(s), {} after the ranking, candles at {} ms, {} closure poll(s), saved at {} ms{}",
            formatYMD(targetDate), elapsedMs(), earlyPlan.size(), firstMs, rankingRetries, remaining.size(), candlesMs,
            closurePolls, elapsedMs(), usePrepared ? " (prepared before midnight)" : "");

    return summary;
}
