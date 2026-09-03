#include "database_downloader.h"
#include "logger.h"
#include "database_utils.h"        // writeCallback declaration

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <algorithm>
#include <string>
#include <set>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <exception>

#include "time_utils.h"

using json = nlohmann::json;

namespace {

constexpr long HTTP_CONNECT_TIMEOUT_MS = 5000;
constexpr long HTTP_REQUEST_TIMEOUT_MS = 15000;
constexpr int HTTP_MAX_ATTEMPTS = 3;
constexpr int HTTP_INITIAL_BACKOFF_MS = 250;

bool performHttpGet(
    const std::string& url,
    std::string& response,
    const std::string& requestName)
{
    for (int attempt = 1; attempt <= HTTP_MAX_ATTEMPTS; ++attempt)
    {
        response.clear();

        CURL* curl = curl_easy_init();
        if (!curl) {
            LG_ERROR("[{}] Failed to initialize CURL", requestName);
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_REQUEST_TIMEOUT_MS);

        CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300)
            return true;

        bool retryable =
            rc != CURLE_OK ||
            httpCode == 408 ||
            httpCode == 429 ||
            httpCode >= 500;

        if (rc != CURLE_OK) {
            LG_WARN(
                "[{}] HTTP attempt {}/{} failed: {}",
                requestName, attempt, HTTP_MAX_ATTEMPTS, curl_easy_strerror(rc)
            );
        } else {
            LG_WARN(
                "[{}] HTTP attempt {}/{} returned status {}",
                requestName, attempt, HTTP_MAX_ATTEMPTS, httpCode
            );
        }

        if (!retryable || attempt == HTTP_MAX_ATTEMPTS)
            break;

        int backoffMs = HTTP_INITIAL_BACKOFF_MS << (attempt - 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    }

    LG_ERROR("[{}] HTTP request failed", requestName);
    return false;
}

} // namespace

/**************************************************************************************
 * Purpose : Fetches the currently tradable Binance USD-M perpetual symbols quoted in
 *           USDT using the /fapi/v1/exchangeInfo endpoint.
 * Args    : None
 * Return  : std::set<std::string> - Eligible symbols (e.g., "BTCUSDT").
 **************************************************************************************/
std::set<std::string> DatabaseDownloader::getEligiblePerpetualPairs()
{
    std::set<std::string> result;
    std::string response;

    std::string url = binance_base_url_ + "/fapi/v1/exchangeInfo";
    if (!performHttpGet(url, response, "exchangeInfo"))
        return result;

    json exchangeInfo;
    try {
        exchangeInfo = json::parse(response);
    }
    catch (const json::exception& e) {
        LG_ERROR("exchangeInfo JSON parse failed: {}", e.what());
        return result;
    }

    if (!exchangeInfo.is_object() ||
        !exchangeInfo.contains("symbols") ||
        !exchangeInfo["symbols"].is_array())
    {
        LG_ERROR("exchangeInfo returned unexpected JSON shape");
        return result;
    }

    for (const auto& item : exchangeInfo["symbols"])
    {
        if (!item.is_object() ||
            !item.contains("symbol") ||
            !item.contains("contractType") ||
            !item.contains("status") ||
            !item.contains("quoteAsset"))
        {
            LG_WARN("exchangeInfo contains an incomplete symbol entry");
            continue;
        }

        try {
            if (item["contractType"].get<std::string>() != "PERPETUAL" ||
                item["status"].get<std::string>() != "TRADING" ||
                item["quoteAsset"].get<std::string>() != "USDT")
            {
                continue;
            }

            result.insert(item["symbol"].get<std::string>());
        }
        catch (const std::exception& e) {
            LG_WARN("exchangeInfo contains an invalid symbol entry: {}", e.what());
        }
    }

    LG_INFO("exchangeInfo returned {} eligible USDT perpetual symbols", result.size());
    return result;
}

/**************************************************************************************
 * Purpose : Fetches the top-50 Binance USDT perpetual futures pairs by 24h quote 
 *           volume using the /fapi/v1/ticker/24hr endpoint.
 * Args    : None
 * Return  : std::set<std::string> - Set of symbols (e.g., "BTCUSDT").
 **************************************************************************************/
std::set<std::string> DatabaseDownloader::getTop50PairsByVolume()
{
    std::set<std::string> result;

    std::set<std::string> eligiblePairs = getEligiblePerpetualPairs();
    if (eligiblePairs.empty()) {
        LG_ERROR("exchangeInfo returned no eligible USDT perpetual symbols");
        return result;
    }

    std::string response;
    std::string url = binance_base_url_ + "/fapi/v1/ticker/24hr";
    if (!performHttpGet(url, response, "ticker/24hr"))
        return result;

    json tickers;
    try {
        tickers = json::parse(response);
    }
    catch (const json::exception& e) {
        LG_ERROR("ticker/24hr JSON parse failed: {}", e.what());
        return result;
    }

    if (!tickers.is_array()) {
        LG_ERROR("ticker/24hr returned unexpected JSON shape");
        return result;
    }

    struct PairVolume { std::string symbol; double quoteVol; };
    std::vector<PairVolume> pairs;

    for (auto& item : tickers)
    {
        if (!item.is_object() || !item.contains("symbol") || !item.contains("quoteVolume")) {
            LG_WARN("ticker/24hr contains an item without symbol/quoteVolume");
            continue;
        }

        try {
            std::string symbol = item["symbol"].get<std::string>();

            if (eligiblePairs.contains(symbol))
            {
                double quoteVol = std::stod(item["quoteVolume"].get<std::string>());
                pairs.push_back({symbol, quoteVol});
            }
        }
        catch (const std::exception& e) {
            LG_WARN("ticker/24hr contains an invalid item: {}", e.what());
        }
    }

    std::sort(pairs.begin(), pairs.end(),
              [](auto& a, auto& b) { return a.quoteVol > b.quoteVol; });

    int count = static_cast<int>(std::min<std::size_t>(50, pairs.size()));
    for (int i = 0; i < count; ++i)
        result.insert(pairs[i].symbol);

    return result;
}

/**************************************************************************************
 * Purpose : Computes the last N calendar days ending at the given date.
 *           Dates are returned as compact integers of the form YYYYMMDD.
 *
 * Args    : end - The last day in the sequence.
 *           n   - Number of days to generate (going backwards).
 *
 * Return  : std::vector<int> - A vector of YYYYMMDD values, newest first.
 **************************************************************************************/
static std::vector<int> computeLastNDays(
    std::chrono::year_month_day end,
    int n
){
    std::vector<int> out;
    out.reserve(n);

    std::chrono::sys_days cur = std::chrono::sys_days(end);

    for (int i = 0; i < n; i++)
    {
        auto ymd = std::chrono::year_month_day(cur);
        out.push_back(toYYYYMMDD(ymd));
        cur -= std::chrono::days(1);
    }

    return out;
}

/**************************************************************************************
 * Purpose : Fetch up to 100 days of OHLCV (1d candles) ending exactly at `targetDate`.
 *           Caller guarantees `targetDate` is the last full day (e.g., yesterday).
 *
 * Args    : targetDate   - YYYY-MM-DD date for last complete candle
 *           dataToDownload - map<pair → days to download>
 *
 * Return  : OHLCVData - Structure: result.data[pair][YYYYMMDD] = OHLCV{...}
 **************************************************************************************/
OHLCVData DatabaseDownloader::fetchDataOHLCV(
    std::chrono::year_month_day targetDate,
    const std::map<std::string,int>& dataToDownload)
{
    OHLCVData result;
    std::mutex writeMutex;

    const int targetYmd = toYYYYMMDD(targetDate);
    LG_INFO("TargetDate = {}", targetYmd);

    // Convert map directly to vector for batching
    std::vector<std::string> pairs;
    pairs.reserve(dataToDownload.size());
    for (auto& [p, _] : dataToDownload)
        pairs.push_back(p);

    constexpr std::size_t MAX_THREADS = 8;

    for (std::size_t i = 0; i < pairs.size(); i += MAX_THREADS)
    {
        std::size_t batchEnd = std::min(i + MAX_THREADS, pairs.size());
        std::vector<std::thread> workers;

        for (std::size_t j = i; j < batchEnd; ++j)
        {
            std::string pair = pairs[j];

            workers.emplace_back([&, pair]() {

                int daysNeeded = dataToDownload.at(pair);
                daysNeeded = std::clamp(daysNeeded, 1, 100);

                // -------------------------------
                // Compute time window per pair
                // -------------------------------
                auto endSys   = std::chrono::sys_days(targetDate);                    // target day 00:00
                auto startSys = endSys - std::chrono::days(daysNeeded - 1);           // inclusive

                int startYmd = toYYYYMMDD(std::chrono::year_month_day(startSys));

                // END = next day 00:00 UTC (exclusive upper bound)
                auto nextDaySys = endSys + std::chrono::days(1);
                int endYmd = toYYYYMMDD(std::chrono::year_month_day(nextDaySys));

                long startMs = toUnixMillis(startYmd);
                long endMs   = toUnixMillis(endYmd);

                LG_INFO(
                    "[{}] Fetch {} days: {} → {} (endExclusive={})",
                    pair, daysNeeded, startYmd, targetYmd, endYmd
                );

                // -------------------------------
                // Build Binance request URL
                // -------------------------------
                std::string url = fmt::format(
                    "{}/fapi/v1/klines"
                    "?symbol={}&interval=1d&limit={}&startTime={}&endTime={}",
                    binance_base_url_, pair, daysNeeded, startMs, endMs
                );

                // Perform request
                std::string response;
                if (!performHttpGet(url, response, pair))
                    return;

                // Parse JSON
                json j;
                try {
                    j = json::parse(response);
                }
                catch (const json::exception& e) {
                    LG_ERROR("[{}] JSON parse failed: {}", pair, e.what());
                    return;
                }

                if (!j.is_array()) {
                    LG_ERROR("[{}] Unexpected klines JSON shape", pair);
                    return;
                }

                OHLCVData local;

                for (auto& arr : j)
                {
                    if (!arr.is_array() || arr.size() < 6) {
                        LG_WARN("[{}] Ignoring malformed kline", pair);
                        continue;
                    }

                    try {
                        long openTime = arr[0].get<long>();

                        auto tp_days = std::chrono::floor<std::chrono::days>(
                            std::chrono::system_clock::time_point(
                                std::chrono::milliseconds(openTime))
                        );

                        int ymd = toYYYYMMDD(std::chrono::year_month_day(tp_days));

                        // Extra safety: skip future candles
                        if (ymd > targetYmd)
                            continue;

                        OHLCV c;
                        c.open   = std::stod(arr[1].get<std::string>());
                        c.high   = std::stod(arr[2].get<std::string>());
                        c.low    = std::stod(arr[3].get<std::string>());
                        c.close  = std::stod(arr[4].get<std::string>());
                        c.volume = std::stod(arr[5].get<std::string>());

                        local.data[pair][ymd] = c;
                    }
                    catch (const std::exception& e) {
                        LG_WARN("[{}] Ignoring invalid kline: {}", pair, e.what());
                    }
                }

                // Merge thread-local data safely
                {
                    std::lock_guard<std::mutex> lock(writeMutex);
                    result.data.merge(local.data);
                }

            });
        }

        for (auto& t : workers)
            t.join();
    }

    LG_INFO("fetchDataOHLCV complete.");
    return result;
}
