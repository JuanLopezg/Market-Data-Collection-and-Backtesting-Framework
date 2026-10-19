#pragma once

#include <deque>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "market_data_examples.h"

namespace market_data_test {

// Script the external HTTP transport in memory. Test-only linker wrappers feed
// these bodies through libcurl's write callback; production client parsing,
// ingestion and real SQLite remain in use. No sockets, DNS or HTTP I/O occur.
class FakeBinanceHttp {
public:
    struct Request {
        std::string path;
        std::map<std::string, std::string> query;
    };

    struct Response {
        long status = 404;
        std::string body;
    };

    FakeBinanceHttp();
    ~FakeBinanceHttp();
    FakeBinanceHttp(const FakeBinanceHttp&) = delete;
    FakeBinanceHttp& operator=(const FakeBinanceHttp&) = delete;

    const std::string& url() const { return url_; }
    void set(const std::string& route, const Json& body);
    void script(const std::string& route, std::initializer_list<std::string> bodies);
    std::vector<Request> requests() const { return requests_; }
    void clearRequests() { requests_.clear(); }

    // Used only by the test transport adapter below the real Binance client.
    Response respond(const std::string& url);
    static FakeBinanceHttp* active() { return active_; }

private:
    inline static FakeBinanceHttp* active_ = nullptr;
    const std::string url_ = "http://scripted-binance.invalid";
    std::map<std::string, std::deque<std::string>> responses_;
    std::vector<Request> requests_;
};

} // namespace market_data_test
