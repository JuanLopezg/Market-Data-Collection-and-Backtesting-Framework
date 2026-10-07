// Implements Binance REST calls, response validation, and bounded parallel daily-candle
// downloads.

#include "binance_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>

#include <curl/curl.h>
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
    const std::string& url,
    std::string& response,
    const std::string& name) const
{
    for (int attempt = 1; attempt <= HTTP_MAX_ATTEMPTS; ++attempt) {
        response.clear();
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;

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
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300)
            return true;

        LG_WARN(
            "service=market-data event=binance_http_retry request={} attempt={} max_attempts={} curl_code={} http_code={}",
            name, attempt, HTTP_MAX_ATTEMPTS, static_cast<int>(rc), httpCode
        );

        const bool retryable = rc != CURLE_OK || httpCode == 408 || httpCode == 429 || httpCode >= 500;
        if (!retryable || attempt == HTTP_MAX_ATTEMPTS)
            break;

        const int backoff = HTTP_INITIAL_BACKOFF_MS << (attempt - 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }

    LG_ERROR("service=market-data event=binance_http_failed request={}", name);
    return false;
}

BinanceUniverseSnapshot
BinanceMarketDataClient::universeByQuoteVolume(std::size_t limit) const
{
    std::string response;
    if (!httpGet(base_url_ + "/fapi/v1/exchangeInfo", response, "exchangeInfo"))
        return {};

    json exchangeInfo;
    try {
        exchangeInfo = json::parse(response);
    }
    catch (const std::exception& error) {
        LG_ERROR("service=market-data event=exchange_info_parse_failed error={}", error.what());
        return {};
    }

    BinanceUniverseSnapshot snapshot;
    if (!exchangeInfo.is_object() || !exchangeInfo.contains("symbols") ||
        !exchangeInfo.at("symbols").is_array()) {
        LG_ERROR("service=market-data event=exchange_info_shape_invalid");
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

    response.clear();
    if (!httpGet(base_url_ + "/fapi/v1/ticker/24hr", response, "ticker/24hr"))
        return {};

    json tickers;
    try {
        tickers = json::parse(response);
    }
    catch (const std::exception& error) {
        LG_ERROR("service=market-data event=ticker_parse_failed error={}", error.what());
        return {};
    }

    if (!tickers.is_array()) {
        LG_ERROR("service=market-data event=ticker_shape_invalid");
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

    const std::size_t parallel = std::max<std::size_t>(1, maxParallelRequests);

    for (std::size_t batchStart = 0; batchStart < requests.size(); batchStart += parallel) {
        const std::size_t batchEnd = std::min(batchStart + parallel, requests.size());
        std::vector<std::thread> workers;

        for (std::size_t i = batchStart; i < batchEnd; ++i) {
            const MarketDataDownloadRequest request = requests[i];
            workers.emplace_back([&, request]() {
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
                    if (!httpGet(url, response, "klines:" + request.symbol)) {
                        failed = true;
                        break;
                    }

                    json rows;
                    try {
                        rows = json::parse(response);
                    }
                    catch (const std::exception& error) {
                        LG_ERROR(
                            "service=market-data event=kline_parse_failed symbol={} error={}",
                            request.symbol, error.what()
                        );
                        failed = true;
                        break;
                    }

                    if (!rows.is_array()) {
                        failed = true;
                        break;
                    }

                    for (const auto& row : rows) {
                        if (!row.is_array() || row.size() < 6)
                            continue;
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
                            local.data[request.symbol][static_cast<Timestamp>(toYYYYMMDD(date))] = bar;
                        }
                        catch (const std::exception& error) {
                            LG_WARN(
                                "service=market-data event=kline_row_ignored symbol={} error={}",
                                request.symbol, error.what()
                            );
                        }
                    }

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
        }

        for (auto& worker : workers)
            worker.join();
    }

    std::sort(result.failed_symbols.begin(), result.failed_symbols.end());
    result.failed_symbols.erase(
        std::unique(result.failed_symbols.begin(), result.failed_symbols.end()),
        result.failed_symbols.end());
    return result;
}
