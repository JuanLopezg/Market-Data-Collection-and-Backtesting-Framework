#include "binance_prices.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

constexpr long CONNECT_TIMEOUT_MS = 5000;
constexpr long REQUEST_TIMEOUT_MS = 10000;

std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* target)
{
    static_cast<std::string*>(target)->append(data, size * count);
    return size * count;
}

} // namespace


BinanceLastPrices::BinanceLastPrices(std::string baseUrl)
    : base_url_(std::move(baseUrl))
{
    while (!base_url_.empty() && base_url_.back() == '/')
        base_url_.pop_back();
    if (base_url_.empty())
        throw std::invalid_argument("Binance price URL cannot be empty");
}

std::unordered_map<Coin, LastPrice> BinanceLastPrices::fetchAll() const
{
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("Cannot create a libcurl handle");

    const std::string url = base_url_ + "/fapi/v1/ticker/price";
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, REQUEST_TIMEOUT_MS);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "algotrading-execution-state/1.0");

    const CURLcode result = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK)
        throw std::runtime_error(std::string("Binance price request failed: ") + curl_easy_strerror(result));
    if (status != 200)
        throw std::runtime_error("Binance price request answered HTTP " + std::to_string(status));

    // Parse without exceptions so a non-JSON answer fails like every other bad answer.
    const nlohmann::json rows = nlohmann::json::parse(body, nullptr, false);
    if (rows.is_discarded())
        throw std::runtime_error("Binance price answer is not JSON");
    if (!rows.is_array())
        throw std::runtime_error("Binance price answer is not a list");

    std::unordered_map<Coin, LastPrice> prices;
    for (const auto& row : rows) {
        const double price = std::stod(row.at("price").get<std::string>());
        if (!std::isfinite(price) || price <= 0.0)
            continue;
        prices[row.at("symbol").get<std::string>()] = LastPrice{price, row.value("time", std::int64_t{0})};
    }
    return prices;
}
