#include "http_transport.h"

#include <memory>
#include <mutex>
#include <stdexcept>

#include <curl/curl.h>

namespace Venue {

namespace {

struct ResponseBuffer {
    std::string body;
    std::size_t limit = 0;
    bool exceeded = false;
};

std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& buffer = *static_cast<ResponseBuffer*>(user);
    const std::size_t bytes = size * count;
    if (buffer.body.size() + bytes > buffer.limit) {
        buffer.exceeded = true;
        return 0; // Abort the transfer; curl reports a write error.
    }
    buffer.body.append(data, bytes);
    return bytes;
}

void initializeCurlOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("libcurl initialization failed");
    });
}

} // namespace


CurlHttpTransport::CurlHttpTransport(long timeoutMs, std::size_t maxResponseBytes)
    : timeout_ms_(timeoutMs), max_response_bytes_(maxResponseBytes)
{
    if (timeoutMs <= 0 || maxResponseBytes == 0)
        throw std::invalid_argument("HTTP timeout and response limit must be positive");
    initializeCurlOnce();
}


HttpResponse CurlHttpTransport::send(const HttpRequest& request)
{
    if (request.url.rfind("https://", 0) != 0)
        throw std::invalid_argument("Venue requests must use HTTPS");
    if (request.method != "GET" && request.method != "POST")
        throw std::invalid_argument("Unsupported venue HTTP method");

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return {false, 0, {}, "HTTP client unavailable"};

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    for (const auto& [name, value] : request.headers) {
        const std::string line = name + ": " + value;
        curl_slist* extended = curl_slist_append(headers.get(), line.c_str());
        if (extended == nullptr)
            return {false, 0, {}, "HTTP header allocation failed"};
        headers.release();
        headers.reset(extended);
    }

    ResponseBuffer buffer;
    buffer.limit = max_response_bytes_;

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, ""); // Ignore proxy environment variables.
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "algoTrading-venue-adapter");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
    if (request.method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    const CURLcode code = curl_easy_perform(curl.get());
    if (buffer.exceeded)
        return {false, 0, {}, "Venue response exceeded its size bound"};
    if (code != CURLE_OK) {
        // An incomplete exchange: the venue may or may not have processed a POST.
        const std::string reason = code == CURLE_OPERATION_TIMEDOUT ? "timed out" : "failed";
        return {false, 0, {}, "HTTP request to " + request.endpoint + " " + reason};
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return {true, status, std::move(buffer.body), {}};
}


HttpResponse GetOnlyTransport::send(const HttpRequest& request)
{
    if (request.method != "GET")
        throw std::logic_error("Order submission is disabled: refused " + request.method +
                               " to venue endpoint " + request.endpoint);
    return inner_.send(request);
}

} // namespace Venue
