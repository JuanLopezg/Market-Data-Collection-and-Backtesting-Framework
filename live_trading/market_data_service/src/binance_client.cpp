// Implements Binance REST calls, response validation, and bounded parallel daily-candle
// downloads.

#include "binance_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "service_logging.h"
#include "time_utils.h"

namespace {

using json = nlohmann::json;

constexpr long HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr long HTTP_REQUEST_TIMEOUT_MS = 20000;
constexpr int HTTP_MAX_ATTEMPTS = 4;
constexpr int HTTP_INITIAL_BACKOFF_MS = 500;
constexpr int BINANCE_KLINE_PAGE_LIMIT = 1000;
// Right after midnight a symbol's new daily candle appears with its first trade. Thin
// symbols can take a moment, so an open that has not started yet is polled this long.
constexpr auto OPENING_CANDLE_WAIT = std::chrono::seconds(30);
constexpr auto OPENING_CANDLE_POLL = std::chrono::milliseconds(250);

// One libcurl handle owned by one worker thread. Reusing it keeps the TLS connection
// to Binance open, so only the first request of each worker pays the handshake.
class CurlHandle {
public:
    CurlHandle() : curl_(curl_easy_init()) {}
    ~CurlHandle() { if (curl_) curl_easy_cleanup(curl_); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CURL* get() const { return curl_; }
private:
    CURL* curl_;
};

// Runs task(index, curl) for every index in [0, count) on at most maxParallel worker
// threads. Each worker takes the next free index as soon as it finishes one, so a slow
// request never holds back a whole batch.
template <typename Task>
void forEachInParallel(std::size_t count, std::size_t maxParallel, Task task)
{
    std::atomic<std::size_t> next{0};
    const std::size_t workerCount = std::min(std::max<std::size_t>(1, maxParallel), count);
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&] {
            const CurlHandle curl;
            for (std::size_t index = next++; index < count; index = next++)
                task(index, curl.get());
        });
    }
    for (auto& worker : workers)
        worker.join();
}

std::size_t writeCallback(void* contents, std::size_t size, std::size_t nmemb, void* userp)
{
    const std::size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

std::chrono::year_month_day dateFromOpenTime(long long openTimeMs)
{
    const auto tp = std::chrono::system_clock::time_point{std::chrono::milliseconds{openTimeMs}};
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(tp)};
}

long long unixMillis(std::chrono::year_month_day date)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::sys_days{date}.time_since_epoch()).count();
}

} // namespace

BinanceMarketDataClient::BinanceMarketDataClient(std::string baseUrl)
    : base_url_(std::move(baseUrl))
{
    if (!base_url_.empty() && base_url_.back() == '/')
        base_url_.pop_back();
}

bool BinanceMarketDataClient::httpGet(
    CURL* curl,
    const std::string& url,
    std::string& response,
    const std::string& name) const
{
    if (!curl) {
        LG_ERROR("Binance request {} has no HTTP handle", name);
        return false;
    }
    for (int attempt = 1; attempt <= HTTP_MAX_ATTEMPTS; ++attempt) {
        response.clear();
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_REQUEST_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "algotrading-market-data/1.0");

        const CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300)
            return true;

        LG_WARN("Binance request {} failed (attempt {} of {}, curl code {}, HTTP {}); retrying",
                name, attempt, HTTP_MAX_ATTEMPTS, static_cast<int>(rc), httpCode);

        const bool retryable = rc != CURLE_OK || httpCode == 408 || httpCode == 429 || httpCode >= 500;
        if (!retryable || attempt == HTTP_MAX_ATTEMPTS)
            break;

        const int backoff = HTTP_INITIAL_BACKOFF_MS << (attempt - 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }

    LG_ERROR("Binance request {} failed after all retries", name);
    return false;
}

long long BinanceMarketDataClient::serverTimeMs() const
{
    const CurlHandle curl;
    std::string response;
    if (!httpGet(curl.get(), base_url_ + "/fapi/v1/time", response, "time"))
        return 0;
    try {
        const json body = json::parse(response);
        if (body.is_object() && body.contains("serverTime") && body.at("serverTime").is_number_integer())
            return body.at("serverTime").get<long long>();
    }
    catch (const std::exception& error) {
        LG_ERROR("Could not parse Binance server time: {}", error.what());
        return 0;
    }
    LG_ERROR("Binance server time has an unexpected shape");
    return 0;
}

BinanceUniverseSnapshot
BinanceMarketDataClient::universeByQuoteVolume(std::size_t limit) const
{
    // The two large documents are independent: download the 24h tickers while exchangeInfo loads.
    auto tickersDownload = std::async(std::launch::async, [this] {
        const CurlHandle tickersCurl;
        std::string body;
        const bool ok = httpGet(tickersCurl.get(), base_url_ + "/fapi/v1/ticker/24hr", body, "ticker/24hr");
        return ok ? body : std::string();
    });

    const CurlHandle curl;
    std::string response;
    if (!httpGet(curl.get(), base_url_ + "/fapi/v1/exchangeInfo", response, "exchangeInfo"))
        return {};

    json exchangeInfo;
    try {
        exchangeInfo = json::parse(response);
    }
    catch (const std::exception& error) {
        LG_ERROR("Could not parse Binance exchange info: {}", error.what());
        return {};
    }

    BinanceUniverseSnapshot snapshot;
    if (!exchangeInfo.is_object() || !exchangeInfo.contains("symbols") ||
        !exchangeInfo.at("symbols").is_array()) {
        LG_ERROR("Binance exchange info has an unexpected shape");
        return {};
    }
    for (const auto& item : exchangeInfo.at("symbols")) {
        try {
            if (item.at("contractType").get<std::string>() == "PERPETUAL" &&
                item.at("status").get<std::string>() == "TRADING" &&
                item.at("quoteAsset").get<std::string>() == "USDT") {
                snapshot.eligible_symbols.insert(item.at("symbol").get<std::string>());
            }
        }
        catch (...) {
            // Ignore malformed symbol entries, but continue with valid exchangeInfo rows.
        }
    }

    response = tickersDownload.get();
    if (response.empty())
        return {};

    json tickers;
    try {
        tickers = json::parse(response);
    }
    catch (const std::exception& error) {
        LG_ERROR("Could not parse Binance 24h tickers: {}", error.what());
        return {};
    }

    if (!tickers.is_array()) {
        LG_ERROR("Binance 24h tickers have an unexpected shape");
        return {};
    }

    std::vector<std::pair<std::string, double>> ranked;
    for (const auto& item : tickers) {
        try {
            const std::string symbol = item.at("symbol").get<std::string>();
            if (!snapshot.eligible_symbols.contains(symbol))
                continue;
            const double quoteVolume = std::stod(item.at("quoteVolume").get<std::string>());
            if (std::isfinite(quoteVolume) && quoteVolume >= 0.0)
                ranked.emplace_back(symbol, quoteVolume);
        }
        catch (...) {
            // Ignore malformed ticker rows.
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second)
            return left.second > right.second;
        return left.first < right.first;
    });

    if (ranked.size() > limit)
        ranked.resize(limit);

    snapshot.ranked_top = std::move(ranked);
    return snapshot;
}

BinanceFetchResult BinanceMarketDataClient::fetchDailyBars(
    const std::vector<MarketDataDownloadRequest>& requests,
    std::size_t maxParallelRequests) const
{
    BinanceFetchResult result;
    std::mutex mutex;

    forEachInParallel(requests.size(), maxParallelRequests, [&](std::size_t index, CURL* curl) {
        const MarketDataDownloadRequest& request = requests[index];
        OHLCVData local;
        bool failed = false;

        auto pageStart = std::chrono::sys_days{request.start_date};
        const auto finalDay = std::chrono::sys_days{request.end_date};

        while (pageStart <= finalDay && !failed) {
            const auto remainingDays = (finalDay - pageStart).count() + 1;
            const int pageDays = static_cast<int>(
                std::min<long long>(BINANCE_KLINE_PAGE_LIMIT, remainingDays));
            const auto pageEnd = pageStart + std::chrono::days(pageDays - 1);

            const auto startDate = std::chrono::year_month_day{pageStart};
            const auto endExclusiveDate = std::chrono::year_month_day{pageEnd + std::chrono::days(1)};

            const std::string url =
                base_url_ + "/fapi/v1/klines?symbol=" + request.symbol +
                "&interval=1d&limit=" + std::to_string(pageDays) +
                "&startTime=" + std::to_string(unixMillis(startDate)) +
                "&endTime=" + std::to_string(unixMillis(endExclusiveDate));

            std::string response;
            if (!httpGet(curl, url, response, "klines:" + request.symbol)) {
                failed = true;
                break;
            }

            json rows;
            try {
                rows = json::parse(response);
            }
            catch (const std::exception& error) {
                LG_ERROR("Could not parse Binance candles for {}: {}", request.symbol, error.what());
                failed = true;
                break;
            }

            if (!rows.is_array()) {
                failed = true;
                break;
            }

            for (const auto& row : rows) {
                if (!row.is_array() || row.size() < 8) {
                    failed = true;
                    break;
                }
                try {
                    const long long openTime = row.at(0).get<long long>();
                    const auto date = dateFromOpenTime(openTime);
                    if (std::chrono::sys_days{date} < std::chrono::sys_days{request.start_date} ||
                        std::chrono::sys_days{date} > finalDay)
                        continue;

                    OHLCV bar;
                    bar.open = std::stod(row.at(1).get<std::string>());
                    bar.high = std::stod(row.at(2).get<std::string>());
                    bar.low = std::stod(row.at(3).get<std::string>());
                    bar.close = std::stod(row.at(4).get<std::string>());
                    bar.volume = std::stod(row.at(5).get<std::string>());
                    // USD-M kline field 7 is actual quote-asset turnover, not volume * close.
                    bar.quote_volume = std::stod(row.at(7).get<std::string>());
                    if (!std::isfinite(bar.quote_volume) || bar.quote_volume < 0.0)
                        throw std::runtime_error("Invalid kline quote volume");
                    local.data[request.symbol][static_cast<Timestamp>(toYYYYMMDD(date))] = bar;
                }
                catch (const std::exception& error) {
                    LG_WARN("Skipped invalid Binance candle for {}: {}", request.symbol, error.what());
                    failed = true;
                    break;
                }
            }

            if (failed) break;

            pageStart = pageEnd + std::chrono::days(1);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (failed) {
            result.failed_symbols.push_back(request.symbol);
            return;
        }
        for (const auto& [symbol, rows] : local.data) {
            auto& destination = result.bars.data[symbol];
            destination.insert(rows.begin(), rows.end());
        }
    });

    std::sort(result.failed_symbols.begin(), result.failed_symbols.end());
    result.failed_symbols.erase(
        std::unique(result.failed_symbols.begin(), result.failed_symbols.end()),
        result.failed_symbols.end());
    return result;
}

std::unordered_map<Coin, double> BinanceMarketDataClient::fetchOpeningPrices(
    std::chrono::year_month_day date,
    const std::vector<std::string>& symbols,
    std::size_t maxParallelRequests) const
{
    std::unordered_map<Coin, double> prices;
    std::mutex mutex;
    bool failed = false;
    const auto start = unixMillis(date);
    forEachInParallel(symbols.size(), maxParallelRequests, [&](std::size_t index, CURL* curl) {
        const std::string& symbol = symbols[index];
        try {
            const std::string url = base_url_ + "/fapi/v1/klines?symbol=" + symbol +
                "&interval=1d&limit=1&startTime=" + std::to_string(start) +
                "&endTime=" + std::to_string(start + 86400000 - 1);
            const auto giveUpAt = std::chrono::steady_clock::now() + OPENING_CANDLE_WAIT;
            json rows;
            while (true) {
                std::string response;
                if (!httpGet(curl, url, response, "paper-open:" + symbol))
                    throw std::runtime_error("Paper opening-price request failed");
                rows = json::parse(response);
                // An empty answer means the candle has not started yet (no trade since midnight).
                const bool notStartedYet = rows.is_array() && rows.empty();
                if (!notStartedYet || std::chrono::steady_clock::now() >= giveUpAt)
                    break;
                std::this_thread::sleep_for(OPENING_CANDLE_POLL);
            }
            if (!rows.is_array() || rows.empty() || !rows.at(0).is_array() || rows.at(0).size() < 2 ||
                rows.at(0).at(0).get<long long>() != start)
                throw std::runtime_error("Paper opening-price candle is absent or misdated");
            const double price = std::stod(rows.at(0).at(1).get<std::string>());
            if (!std::isfinite(price) || price <= 0)
                throw std::runtime_error("Paper opening price must be finite and positive");
            std::lock_guard<std::mutex> lock(mutex);
            prices.emplace(symbol, price);
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            LG_WARN("Opening price for {} is not available yet; will retry this cycle", symbol);
        }
    });
    if (failed || prices.size() != symbols.size())
        throw std::runtime_error("Paper execution requires a complete, correctly dated opening-price snapshot");
    return prices;
}
