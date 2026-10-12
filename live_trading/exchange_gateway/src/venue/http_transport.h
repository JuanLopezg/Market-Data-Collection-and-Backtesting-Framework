#pragma once

#include <string>
#include <utility>
#include <vector>

// HTTP boundary shared by every real venue.
//
// Venue protocols build complete requests (URL, headers, signed body); the transport only
// moves bytes. Tests replace it with recorded responses, so no venue is contacted offline.
namespace Venue {

struct HttpRequest {
    std::string method;   // "GET" or "POST"
    std::string url;      // Full HTTPS URL including any query string.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;     // application/x-www-form-urlencoded for POST.
    std::string endpoint; // Short venue endpoint name for logs and the rate budget.
};

struct HttpResponse {
    bool transport_ok = false;   // False: no complete HTTP response (DNS, TLS, timeout, size).
    long status = 0;
    std::string body;
    std::string transport_error; // Our own wording; never headers, URLs with secrets or bodies.
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

// libcurl transport: HTTPS only, no redirects, no proxy taken from the environment,
// bounded connect/total time and response size.
class CurlHttpTransport final : public HttpTransport {
public:
    CurlHttpTransport(long timeoutMs, std::size_t maxResponseBytes);
    HttpResponse send(const HttpRequest& request) override;

private:
    long timeout_ms_;
    std::size_t max_response_bytes_;
};

// Refuses every request that is not a GET.
//
// Used whenever order submission is disabled, so that no code path (including a bug)
// can reach a mutating venue endpoint with read-only operation configured.
class GetOnlyTransport final : public HttpTransport {
public:
    explicit GetOnlyTransport(HttpTransport& inner) : inner_(inner) {}
    HttpResponse send(const HttpRequest& request) override;

private:
    HttpTransport& inner_;
};

} // namespace Venue
