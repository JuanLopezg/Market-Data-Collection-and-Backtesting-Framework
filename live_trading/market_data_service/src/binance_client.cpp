// Implements Binance REST calls, response validation, and daily-candle downloads multiplexed
// over one HTTP/2 connection.

#include "binance_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
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

// One libcurl handle for a single request sequence. Reusing it keeps the TLS connection
// to Binance open.
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

std::size_t writeCallback(void* contents, std::size_t size, std::size_t nmemb, void* userp)
{
    const std::size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// Options shared by every Binance request.
void setRequestOptions(CURL* curl, const std::string& url, std::string* response)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_REQUEST_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "algotrading-market-data/1.0");
    // Every encoding libcurl supports (gzip at least). exchangeInfo shrinks from 1.1 MB
    // to 52 KB and the 24h tickers from 295 KB to 65 KB: about 1 s less per daily run.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

bool retryableFailure(CURLcode rc, long httpCode)
{
    return rc != CURLE_OK || httpCode == 408 || httpCode == 429 || httpCode >= 500;
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
    multi_ = curl_multi_init();
    if (multi_)
        curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
}

BinanceMarketDataClient::~BinanceMarketDataClient()
{
    if (multi_)
        curl_multi_cleanup(multi_);
}

void BinanceMarketDataClient::warmUp() const
{
    const auto body = getAll({base_url_ + "/fapi/v1/time"}, 1, "warm-up");
    if (!body[0])
        LG_WARN("Binance connection warm-up failed; the daily run opens its own connection");
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
        setRequestOptions(curl, url, &response);

        const CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300)
            return true;

        LG_WARN("Binance request {} failed (attempt {} of {}, curl code {}, HTTP {}); retrying",
                name, attempt, HTTP_MAX_ATTEMPTS, static_cast<int>(rc), httpCode);

        if (!retryableFailure(rc, httpCode) || attempt == HTTP_MAX_ATTEMPTS)
            break;

        const int backoff = HTTP_INITIAL_BACKOFF_MS << (attempt - 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
    }

    LG_ERROR("Binance request {} failed after all retries", name);
    return false;
}

std::vector<std::optional<std::string>> BinanceMarketDataClient::getAll(
    const std::vector<std::string>& urls, std::size_t maxInFlight, const std::string& name) const
{
    std::vector<std::optional<std::string>> bodies(urls.size());
    std::vector<std::size_t> pending(urls.size());
    for (std::size_t index = 0; index < urls.size(); ++index)
        pending[index] = index;
    maxInFlight = std::max<std::size_t>(1, maxInFlight);
    // One connection carries every request as an HTTP/2 stream (CURLOPT_PIPEWAIT below) and
    // stays open in multi_ for the next call. No connection cap: without HTTP/2 (a
    // plain-HTTP test fixture) curl opens more.
    const std::lock_guard<std::mutex> lock(multi_mutex_);
    CURLM* multi = multi_;
    if (!multi) {
        LG_ERROR("Binance requests {}: no HTTP multi handle", name);
        return bodies;
    }

    for (int attempt = 1; attempt <= HTTP_MAX_ATTEMPTS && !pending.empty(); ++attempt) {
        if (attempt > 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(HTTP_INITIAL_BACKOFF_MS << (attempt - 2)));

        struct Transfer {
            std::size_t index;
            CurlHandle curl;
            std::string response;
        };
        std::map<CURL*, std::unique_ptr<Transfer>> active;
        std::size_t next = 0;
        std::vector<std::size_t> retry;
        const auto addNext = [&] {
            const std::size_t index = pending[next++];
            auto transfer = std::make_unique<Transfer>();
            transfer->index = index;
            CURL* curl = transfer->curl.get();
            setRequestOptions(curl, urls[index], &transfer->response);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
            // Wait for the shared connection instead of opening another one.
            curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
            curl_multi_add_handle(multi, curl);
            active.emplace(curl, std::move(transfer));
        };
        while (next < pending.size() && active.size() < maxInFlight)
            addNext();

        int running = 0;
        do {
            curl_multi_perform(multi, &running);
            int queued = 0;
            while (CURLMsg* message = curl_multi_info_read(multi, &queued)) {
                if (message->msg != CURLMSG_DONE)
                    continue;
                CURL* curl = message->easy_handle;
                const CURLcode rc = message->data.result;
                long httpCode = 0;
                if (rc == CURLE_OK)
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                auto found = active.find(curl);
                const std::size_t index = found->second->index;
                if (rc == CURLE_OK && httpCode >= 200 && httpCode < 300) {
                    bodies[index] = std::move(found->second->response);
                } else {
                    LG_WARN("Binance request {} failed (attempt {} of {}, curl code {}, HTTP {})",
                            name + ":" + std::to_string(index), attempt, HTTP_MAX_ATTEMPTS, static_cast<int>(rc), httpCode);
                    if (retryableFailure(rc, httpCode))
                        retry.push_back(index);
                }
                curl_multi_remove_handle(multi, curl);
                active.erase(found);
                if (next < pending.size())
                    addNext();
            }
            if (!active.empty())
                curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        } while (!active.empty());

        pending = std::move(retry);
    }
    if (!pending.empty())
        LG_ERROR("Binance requests {}: {} failed after all retries", name, pending.size());
    return bodies;
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

std::vector<std::string> BinanceMarketDataClient::universeUrls() const
{
    return {base_url_ + "/fapi/v1/exchangeInfo", base_url_ + "/fapi/v1/ticker/24hr"};
}

BinanceUniverseSnapshot BinanceMarketDataClient::parseUniverse(
    const std::optional<std::string>& exchangeInfoBody,
    const std::optional<std::string>& tickersBody,
    std::size_t limit) const
{
    if (!exchangeInfoBody || !tickersBody)
        return {};

    json exchangeInfo;
    try {
        exchangeInfo = json::parse(*exchangeInfoBody);
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

    json tickers;
    try {
        tickers = json::parse(*tickersBody);
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
            if (item.contains("closeTime") && item.at("closeTime").is_number_integer())
                snapshot.tickers_until_ms = std::max(snapshot.tickers_until_ms, item.at("closeTime").get<long long>());
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

BinanceUniverseSnapshot
BinanceMarketDataClient::universeByQuoteVolume(std::size_t limit) const
{
    // The two documents are independent: both download as streams of one connection.
    const auto documents = getAll(universeUrls(), 2, "exchangeInfo+ticker/24hr");
    return parseUniverse(documents[0], documents[1], limit);
}

std::vector<std::string> BinanceMarketDataClient::klineUrls(
    const std::vector<MarketDataDownloadRequest>& requests, bool nextOpen, std::vector<std::size_t>& owner) const
{
    // Every page of every request is known up front (at most 1000 days per page), so all
    // of them download together.
    std::vector<std::string> urls;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const MarketDataDownloadRequest& request = requests[index];
        auto pageStart = std::chrono::sys_days{request.start_date};
        const auto finalDay = std::chrono::sys_days{request.end_date};
        while (pageStart <= finalDay) {
            const auto remainingDays = (finalDay - pageStart).count() + 1;
            int pageDays = static_cast<int>(std::min<long long>(BINANCE_KLINE_PAGE_LIMIT, remainingDays));
            const auto pageEnd = pageStart + std::chrono::days(pageDays - 1);
            const bool lastPage = pageEnd == finalDay;
            if (lastPage && nextOpen)
                ++pageDays; // the next day's candle: its open, and the proof the end date closed
            const auto startDate = std::chrono::year_month_day{pageStart};
            const auto endExclusiveDate = std::chrono::year_month_day{pageStart + std::chrono::days(pageDays)};
            urls.push_back(base_url_ + "/fapi/v1/klines?symbol=" + request.symbol +
                           "&interval=1d&limit=" + std::to_string(pageDays) +
                           "&startTime=" + std::to_string(unixMillis(startDate)) +
                           "&endTime=" + std::to_string(unixMillis(endExclusiveDate) - 1));
            owner.push_back(index);
            pageStart = pageEnd + std::chrono::days(1);
        }
    }
    return urls;
}

BinanceFetchResult BinanceMarketDataClient::parseDailyBars(
    const std::vector<MarketDataDownloadRequest>& requests,
    const std::vector<std::size_t>& owner,
    const std::vector<std::optional<std::string>>& bodies,
    std::size_t firstBody) const
{
    std::vector<bool> failed(requests.size(), false);
    std::vector<OHLCVData> local(requests.size());
    std::unordered_map<Coin, double> nextOpens;
    for (std::size_t page = 0; page < owner.size(); ++page) {
        const std::size_t index = owner[page];
        const MarketDataDownloadRequest& request = requests[index];
        const std::optional<std::string>& body = bodies[firstBody + page];
        if (failed[index])
            continue;
        if (!body) {
            failed[index] = true;
            continue;
        }

        json rows;
        try {
            rows = json::parse(*body);
        }
        catch (const std::exception& error) {
            LG_ERROR("Could not parse Binance candles for {}: {}", request.symbol, error.what());
            failed[index] = true;
            continue;
        }
        if (!rows.is_array()) {
            failed[index] = true;
            continue;
        }

        const auto nextDay = std::chrono::sys_days{request.end_date} + std::chrono::days(1);
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() < 8) {
                failed[index] = true;
                break;
            }
            try {
                const long long openTime = row.at(0).get<long long>();
                const auto date = dateFromOpenTime(openTime);
                if (std::chrono::sys_days{date} == nextDay) {
                    // Only the open of the started day: its high/low/close are unfinished.
                    const double open = std::stod(row.at(1).get<std::string>());
                    if (!std::isfinite(open) || open <= 0.0)
                        throw std::runtime_error("Invalid next-day open");
                    nextOpens[request.symbol] = open;
                    continue;
                }
                if (std::chrono::sys_days{date} < std::chrono::sys_days{request.start_date} ||
                    std::chrono::sys_days{date} > std::chrono::sys_days{request.end_date})
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
                local[index].data[request.symbol][static_cast<Timestamp>(toYYYYMMDD(date))] = bar;
            }
            catch (const std::exception& error) {
                LG_WARN("Skipped invalid Binance candle for {}: {}", request.symbol, error.what());
                failed[index] = true;
                break;
            }
        }
    }

    BinanceFetchResult result;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const std::string& symbol = requests[index].symbol;
        if (failed[index]) {
            result.failed_symbols.push_back(symbol);
            continue;
        }
        for (const auto& [coin, rows] : local[index].data)
            result.bars.data[coin].insert(rows.begin(), rows.end());
        if (const auto open = nextOpens.find(symbol); open != nextOpens.end())
            result.next_opens[symbol] = open->second;
    }
    std::sort(result.failed_symbols.begin(), result.failed_symbols.end());
    result.failed_symbols.erase(
        std::unique(result.failed_symbols.begin(), result.failed_symbols.end()),
        result.failed_symbols.end());
    return result;
}

BinanceFetchResult BinanceMarketDataClient::fetchDailyBars(
    const std::vector<MarketDataDownloadRequest>& requests,
    std::size_t maxParallelRequests) const
{
    std::vector<std::size_t> owner;
    const auto urls = klineUrls(requests, false, owner);
    return parseDailyBars(requests, owner, getAll(urls, maxParallelRequests, "klines"), 0);
}

BinanceDailySnapshot BinanceMarketDataClient::fetchDailySnapshot(
    const std::vector<MarketDataDownloadRequest>& requests,
    bool withUniverse,
    std::size_t rankingLimit,
    std::size_t maxParallelRequests) const
{
    std::vector<std::string> urls;
    if (withUniverse)
        urls = universeUrls();
    const std::size_t firstKline = urls.size();
    std::vector<std::size_t> owner;
    const auto klines = klineUrls(requests, true, owner);
    urls.insert(urls.end(), klines.begin(), klines.end());

    const auto bodies = getAll(urls, maxParallelRequests, "daily");
    BinanceDailySnapshot snapshot;
    if (withUniverse)
        snapshot.universe = parseUniverse(bodies[0], bodies[1], rankingLimit);
    snapshot.candles = parseDailyBars(requests, owner, bodies, firstKline);
    return snapshot;
}

std::unordered_map<Coin, double> BinanceMarketDataClient::fetchOpeningPrices(
    std::chrono::year_month_day date,
    const std::vector<std::string>& symbols,
    std::size_t maxParallelRequests) const
{
    std::unordered_map<Coin, double> prices;
    const auto start = unixMillis(date);
    const auto giveUpAt = std::chrono::steady_clock::now() + OPENING_CANDLE_WAIT;
    std::vector<std::string> waiting = symbols;
    bool failed = false;
    while (!waiting.empty()) {
        std::vector<std::string> urls;
        for (const auto& symbol : waiting)
            urls.push_back(base_url_ + "/fapi/v1/klines?symbol=" + symbol +
                           "&interval=1d&limit=1&startTime=" + std::to_string(start) +
                           "&endTime=" + std::to_string(start + 86400000 - 1));
        const auto bodies = getAll(urls, maxParallelRequests, "paper-open");
        const bool canWait = std::chrono::steady_clock::now() < giveUpAt;
        std::vector<std::string> notStarted;
        for (std::size_t index = 0; index < waiting.size(); ++index) {
            const std::string& symbol = waiting[index];
            try {
                if (!bodies[index])
                    throw std::runtime_error("Paper opening-price request failed");
                const json rows = json::parse(*bodies[index]);
                // An empty answer means the candle has not started yet (no trade since midnight).
                if (rows.is_array() && rows.empty() && canWait) {
                    notStarted.push_back(symbol);
                    continue;
                }
                if (!rows.is_array() || rows.empty() || !rows.at(0).is_array() || rows.at(0).size() < 2 ||
                    rows.at(0).at(0).get<long long>() != start)
                    throw std::runtime_error("Paper opening-price candle is absent or misdated");
                const double price = std::stod(rows.at(0).at(1).get<std::string>());
                if (!std::isfinite(price) || price <= 0)
                    throw std::runtime_error("Paper opening price must be finite and positive");
                prices.emplace(symbol, price);
            } catch (const std::exception&) {
                failed = true;
                LG_WARN("Opening price for {} is not available yet; will retry this cycle", symbol);
            }
        }
        waiting = std::move(notStarted);
        if (!waiting.empty())
            std::this_thread::sleep_for(OPENING_CANDLE_POLL);
    }
    if (failed || prices.size() != symbols.size())
        throw std::runtime_error("Paper execution requires a complete, correctly dated opening-price snapshot");
    return prices;
}

std::vector<FundingRatePage> BinanceMarketDataClient::fetchFundingRatePages(
    const std::vector<FundingRateRequest>& requests,
    std::size_t maxParallelRequests) const
{
    std::vector<std::string> urls;
    for (const auto& request : requests)
        urls.push_back(base_url_ + "/fapi/v1/fundingRate?symbol=" + request.symbol +
                       "&startTime=" + std::to_string(request.start_ms) +
                       "&limit=" + std::to_string(FUNDING_PAGE_LIMIT));
    const auto bodies = getAll(urls, maxParallelRequests, "funding");

    std::vector<FundingRatePage> pages(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        FundingRatePage& page = pages[index];
        const FundingRateRequest& request = requests[index];
        try {
            if (!bodies[index])
                throw std::runtime_error("request failed");
            const json rows = json::parse(*bodies[index]);
            if (!rows.is_array())
                throw std::runtime_error("answer is not an array");
            for (const auto& row : rows) {
                FundingRateEvent event;
                event.funding_time_ms = row.at("fundingTime").get<long long>();
                event.rate = std::stod(row.at("fundingRate").get<std::string>());
                // Old settlements have an empty markPrice.
                const std::string mark = row.contains("markPrice") && row.at("markPrice").is_string()
                    ? row.at("markPrice").get<std::string>() : std::string{};
                event.mark_price = mark.empty() ? std::nan("") : std::stod(mark);
                if (event.funding_time_ms < request.start_ms || !std::isfinite(event.rate))
                    throw std::runtime_error("invalid or misdated settlement");
                if (!page.events.empty() && event.funding_time_ms <= page.events.back().funding_time_ms)
                    throw std::runtime_error("settlements are not in ascending time order");
                page.events.push_back(event);
            }
        }
        catch (const std::exception& error) {
            LG_WARN("Skipped Binance funding rates of {}: {}", request.symbol, error.what());
            page = FundingRatePage{};
            page.failed = true;
        }
    }
    return pages;
}
