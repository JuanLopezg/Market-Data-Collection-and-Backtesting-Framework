#include "fake_binance_http.h"

#include <cstdarg>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

namespace market_data_test {

FakeBinanceHttp::FakeBinanceHttp()
{
    if (active_)
        throw std::logic_error("Only one scripted Binance transport may be active");
    set("/fapi/v1/time", Json{{"serverTime", JAN3_MS}});
    set("/fapi/v1/exchangeInfo", Json{{"serverTime", JAN2_MS},
        {"symbols", Json::array({symbol("BTCUSDT")})}});
    set("/fapi/v1/ticker/24hr", Json::array({ticker("BTCUSDT", "731")}));
    set("/fapi/v1/klines:BTCUSDT", Json::array({kline(JAN2_MS), nextOpen()}));
    set("/fapi/v1/fundingRate", Json::array());
    active_ = this;
}

FakeBinanceHttp::~FakeBinanceHttp()
{
    active_ = nullptr;
}

void FakeBinanceHttp::set(const std::string& route, const Json& body)
{
    script(route, {body.dump()});
}

void FakeBinanceHttp::script(const std::string& route, std::initializer_list<std::string> bodies)
{
    if (bodies.size() == 0)
        throw std::invalid_argument("Scripted Binance route needs a response");
    responses_[route] = std::deque<std::string>(bodies);
}

FakeBinanceHttp::Response FakeBinanceHttp::respond(const std::string& url)
{
    if (!url.starts_with(url_ + "/"))
        throw std::runtime_error("Unexpected URL in scripted Binance transport: " + url);
    const std::string target = url.substr(url_.size());
    Request request;
    const auto question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string::npos) {
        std::size_t position = question + 1;
        while (position < target.size()) {
            const auto end = target.find('&', position);
            const auto part = target.substr(position, end == std::string::npos ? end : end - position);
            const auto equals = part.find('=');
            if (equals != std::string::npos)
                request.query[part.substr(0, equals)] = part.substr(equals + 1);
            if (end == std::string::npos)
                break;
            position = end + 1;
        }
    }
    requests_.push_back(request);
    auto response = responses_.end();
    if (const auto item = request.query.find("symbol"); item != request.query.end())
        response = responses_.find(request.path + ":" + item->second);
    if (response == responses_.end())
        response = responses_.find(request.path);
    if (response == responses_.end())
        return {}; // Non-retryable missing route, never a real HTTP request.
    Response result{200, response->second.front()};
    if (response->second.size() > 1)
        response->second.pop_front();
    return result;
}

} // namespace market_data_test

namespace {

// Only the libcurl calls used by this core are linked to these test adapters.
// Unknown options/info fail explicitly so an API change cannot silently use I/O.
struct ScriptedEasy {
    std::string url;
    // BinanceMarketDataClient supplies the libcurl-supported void* callback form.
    using Writer = std::size_t (*)(void*, std::size_t, std::size_t, void*);
    Writer write = nullptr;
    void* output = nullptr;
    long status = 0;
};

struct ScriptedMulti {
    std::vector<CURL*> pending;
    std::deque<CURLMsg> completed;
    CURLMsg last{};
};

ScriptedEasy& easy(CURL* handle) { return *reinterpret_cast<ScriptedEasy*>(handle); }
ScriptedMulti& multi(CURLM* handle) { return *reinterpret_cast<ScriptedMulti*>(handle); }

CURLcode deliver(CURL* handle)
{
    auto* endpoint = market_data_test::FakeBinanceHttp::active();
    if (!endpoint)
        throw std::logic_error("Binance HTTP called outside a scripted test");
    auto response = endpoint->respond(easy(handle).url);
    easy(handle).status = response.status;
    if (!easy(handle).write)
        return CURLE_WRITE_ERROR;
    const auto size = response.body.size();
    const auto written = easy(handle).write(response.body.data(), 1, size, easy(handle).output);
    return written == size ? CURLE_OK : CURLE_WRITE_ERROR;
}

} // namespace

extern "C" {

CURL* __wrap_curl_easy_init()
{
    return reinterpret_cast<CURL*>(new ScriptedEasy);
}

void __wrap_curl_easy_cleanup(CURL* handle)
{
    delete reinterpret_cast<ScriptedEasy*>(handle);
}

CURLcode __wrap_curl_easy_setopt(CURL* handle, CURLoption option, ...)
{
    va_list args;
    va_start(args, option);
    CURLcode result = CURLE_OK;
    switch (option) {
    case CURLOPT_URL:
        easy(handle).url = va_arg(args, const char*);
        break;
    case CURLOPT_WRITEFUNCTION:
        easy(handle).write = va_arg(args, ScriptedEasy::Writer);
        break;
    case CURLOPT_WRITEDATA:
        easy(handle).output = va_arg(args, void*);
        break;
    case CURLOPT_USERAGENT:
    case CURLOPT_ACCEPT_ENCODING:
    case CURLOPT_SSL_VERIFYPEER:
    case CURLOPT_SSL_VERIFYHOST:
    case CURLOPT_NOSIGNAL:
    case CURLOPT_CONNECTTIMEOUT_MS:
    case CURLOPT_TIMEOUT_MS:
    case CURLOPT_HTTP_VERSION:
    case CURLOPT_PIPEWAIT:
        break;
    default:
        result = CURLE_UNKNOWN_OPTION;
        break;
    }
    va_end(args);
    if (result != CURLE_OK)
        throw std::logic_error("Unscripted libcurl easy option");
    return result;
}

CURLcode __wrap_curl_easy_perform(CURL* handle) { return deliver(handle); }

CURLcode __wrap_curl_easy_getinfo(CURL* handle, CURLINFO info, ...)
{
    if (info != CURLINFO_RESPONSE_CODE)
        throw std::logic_error("Unscripted libcurl response info");
    va_list args;
    va_start(args, info);
    *va_arg(args, long*) = easy(handle).status;
    va_end(args);
    return CURLE_OK;
}

CURLM* __wrap_curl_multi_init()
{
    return reinterpret_cast<CURLM*>(new ScriptedMulti);
}

CURLMcode __wrap_curl_multi_cleanup(CURLM* handle)
{
    delete reinterpret_cast<ScriptedMulti*>(handle);
    return CURLM_OK;
}

CURLMcode __wrap_curl_multi_setopt(CURLM*, CURLMoption option, ...)
{
    if (option != CURLMOPT_PIPELINING)
        throw std::logic_error("Unscripted libcurl multi option");
    return CURLM_OK;
}

CURLMcode __wrap_curl_multi_add_handle(CURLM* handle, CURL* transfer)
{
    multi(handle).pending.push_back(transfer);
    return CURLM_OK;
}

CURLMcode __wrap_curl_multi_remove_handle(CURLM*, CURL*) { return CURLM_OK; }

CURLMcode __wrap_curl_multi_perform(CURLM* handle, int* running)
{
    auto& state = multi(handle);
    for (CURL* transfer : state.pending) {
        CURLMsg message{};
        message.msg = CURLMSG_DONE;
        message.easy_handle = transfer;
        message.data.result = deliver(transfer);
        state.completed.push_back(message);
    }
    state.pending.clear();
    *running = 0;
    return CURLM_OK;
}

CURLMsg* __wrap_curl_multi_info_read(CURLM* handle, int* queued)
{
    auto& state = multi(handle);
    *queued = static_cast<int>(state.completed.size());
    if (state.completed.empty())
        return nullptr;
    state.last = state.completed.front();
    state.completed.pop_front();
    *queued = static_cast<int>(state.completed.size());
    return &state.last;
}

CURLMcode __wrap_curl_multi_poll(CURLM*, struct curl_waitfd[], unsigned int, int, int* count)
{
    if (count)
        *count = 0;
    return CURLM_OK;
}

} // extern "C"
