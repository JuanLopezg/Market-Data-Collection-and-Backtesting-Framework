// Declares the Binance REST client used to discover the trading universe and download daily
// candles.

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "data_types.h"
#include "market_store.h"

// Result of ranking currently eligible Binance perpetual symbols by quote volume.
struct BinanceUniverseSnapshot {
    std::set<std::string> eligible_symbols;
    std::vector<std::pair<std::string, double>> ranked_top;
    // Newest closeTime of the 24h tickers (Unix ms): the ranking window ends there.
    long long tickers_until_ms = 0;
};

// Daily bars plus the symbols that could not be downloaded successfully.
struct BinanceFetchResult {
    OHLCVData bars;
    std::vector<std::string> failed_symbols;
    // Open of the day after the requested range, per symbol, when Binance already has that
    // candle. Its presence proves the requested last day is closed on Binance's own clock:
    // a candle starts only with the first trade after midnight.
    std::unordered_map<Coin, double> next_opens;
};

// One concurrent download of the daily cycle: the ranking documents (when asked) and the
// candles of the given requests up to the target day plus the next day's open.
struct BinanceDailySnapshot {
    std::optional<BinanceUniverseSnapshot> universe;
    BinanceFetchResult candles;
};

// Small Binance REST adapter used only by the market-data ingestion service.
class BinanceMarketDataClient {
public:
    explicit BinanceMarketDataClient(std::string baseUrl);
    ~BinanceMarketDataClient();
    BinanceMarketDataClient(const BinanceMarketDataClient&) = delete;
    BinanceMarketDataClient& operator=(const BinanceMarketDataClient&) = delete;

    // Binance's current clock from GET /fapi/v1/time (Unix milliseconds), or 0 when it
    // cannot be read. exchangeInfo also carries a serverTime, but that document is
    // served from a cache and can be about 30 minutes old, so it cannot prove a day closed.
    long long serverTimeMs() const;

    // Opens the shared HTTP/2 connection ahead of the daily run (one GET /fapi/v1/time),
    // so the run at midnight pays no DNS or TLS handshake.
    void warmUp() const;

    BinanceUniverseSnapshot universeByQuoteVolume(std::size_t rankingLimit) const;

    BinanceFetchResult fetchDailyBars(
        const std::vector<MarketDataDownloadRequest>& requests,
        std::size_t maxParallelRequests) const;

    // The ranking documents (withUniverse) and every request's candles in one concurrent
    // download. Each request's last page also asks for the day after its end date: that
    // candle's open goes to candles.next_opens and is never stored as a bar.
    BinanceDailySnapshot fetchDailySnapshot(
        const std::vector<MarketDataDownloadRequest>& requests,
        bool withUniverse,
        std::size_t rankingLimit,
        std::size_t maxParallelRequests) const;

    // Read only the first price of an already-started daily candle, never its
    // unfinished high/low/close/volume. A candle that has not started yet (no trade
    // since midnight) is polled briefly; a misdated or still missing open fails the cycle.
    std::unordered_map<Coin, double> fetchOpeningPrices(
        std::chrono::year_month_day date,
        const std::vector<std::string>& symbols,
        std::size_t maxParallelRequests) const;

private:
    std::string base_url_;
    // The multi handle keeps the HTTP/2 connection alive between calls; getAll calls are
    // serialized on it.
    mutable std::mutex multi_mutex_;
    mutable CURLM* multi_ = nullptr;

    // `curl` is a reusable libcurl handle, so consecutive requests share one TLS connection.
    bool httpGet(CURL* curl, const std::string& url, std::string& response, const std::string& name) const;

    // GETs every URL concurrently over one HTTP/2 connection (multiplexed streams), at most
    // maxInFlight at a time, retrying transient failures like httpGet. Measured on the VPS
    // on 2026-10-18: 100 klines in 0.8 s this way, 2.5 s on 16 connections, 3.8 s on one
    // connection each; Binance answers each request in about 0.25 s.
    // bodies[i] is the response of urls[i], or nullopt when it failed after every retry.
    std::vector<std::optional<std::string>> getAll(
        const std::vector<std::string>& urls, std::size_t maxInFlight, const std::string& name) const;

    std::vector<std::string> universeUrls() const;
    BinanceUniverseSnapshot parseUniverse(const std::optional<std::string>& exchangeInfo,
                                          const std::optional<std::string>& tickers, std::size_t limit) const;
    // URLs of every page of every request; owner[i] is the request index of urls[i]. With
    // nextOpen the last page of each request extends one day past its end date.
    std::vector<std::string> klineUrls(const std::vector<MarketDataDownloadRequest>& requests, bool nextOpen,
                                       std::vector<std::size_t>& owner) const;
    BinanceFetchResult parseDailyBars(const std::vector<MarketDataDownloadRequest>& requests,
                                      const std::vector<std::size_t>& owner,
                                      const std::vector<std::optional<std::string>>& bodies,
                                      std::size_t firstBody) const;
};
