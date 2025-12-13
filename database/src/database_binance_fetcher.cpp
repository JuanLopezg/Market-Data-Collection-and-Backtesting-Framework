#include "database_downloader.h"
#include "logger.h"
#include "database.h"        // writeCallback declaration

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <algorithm>
#include <string>
#include <set>
#include <thread>
#include <mutex>
#include <vector>

#include "time_utils.h"

using json = nlohmann::json;

/**************************************************************************************
 * Purpose : Fetches the top-50 Binance USDT perpetual futures pairs by 24h quote 
 *           volume using the /fapi/v1/ticker/24hr endpoint.
 * Args    : None
 * Return  : std::set<std::string> - Set of symbols (e.g., "BTCUSDT").
 **************************************************************************************/
std::set<std::string> DatabaseDownloader::getTop50PairsByVolume()
{
    std::set<std::string> result;
    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        LG_ERROR("Failed to initialize CURL");
        return result;
    }

    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/ticker/24hr");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LG_ERROR("curl_easy_perform failed: {}", curl_easy_strerror(res));
        return result;
    }

    json tickers = json::parse(response);

    struct PairVolume { std::string symbol; double quoteVol; };
    std::vector<PairVolume> pairs;

    for (auto& item : tickers)
    {
        std::string symbol = item["symbol"].get<std::string>();

        // USDT perpetual only
        if (symbol.size() > 4 && symbol.substr(symbol.size() - 4) == "USDT")
        {
            double quoteVol = std::stod(item["quoteVolume"].get<std::string>());
            pairs.push_back({symbol, quoteVol});
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
                    "https://fapi.binance.com/fapi/v1/klines"
                    "?symbol={}&interval=1d&limit={}&startTime={}&endTime={}",
                    pair, daysNeeded, startMs, endMs
                );

                // Perform request
                std::string response;
                CURL* curl = curl_easy_init();
                if (!curl) {
                    LG_ERROR("[{}] CURL init failed", pair);
                    return;
                }

                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

                CURLcode rc = curl_easy_perform(curl);
                curl_easy_cleanup(curl);

                if (rc != CURLE_OK) {
                    LG_ERROR("[{}] curl_easy_perform error: {}",
                                  pair, curl_easy_strerror(rc));
                    return;
                }

                // Parse JSON
                json j;
                try {
                    j = json::parse(response);
                }
                catch (...) {
                    LG_ERROR("[{}] JSON parse failed", pair);
                    return;
                }

                OHLCVData local;

                for (auto& arr : j)
                {
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
