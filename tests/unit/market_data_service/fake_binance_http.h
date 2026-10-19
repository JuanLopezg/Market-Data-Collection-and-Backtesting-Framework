#pragma once

#include <atomic>
#include <cerrno>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "market_data_examples.h"

namespace market_data_test {

// Only the external Binance HTTP boundary is simulated. Each route has scripted
// responses (the final response repeats), with query strings retained for assertions.
// Blocking socket I/O and shutdown provide synchronization; there are no sleeps.
class FakeBinanceHttp {
public:
    struct Request {
        std::string path;
        std::map<std::string, std::string> query;
    };

    FakeBinanceHttp()
    {
        set("/fapi/v1/time", Json{{"serverTime", JAN3_MS}});
        set("/fapi/v1/exchangeInfo", Json{{"serverTime", JAN2_MS},
            {"symbols", Json::array({symbol("BTCUSDT")})}});
        set("/fapi/v1/ticker/24hr", Json::array({ticker("BTCUSDT", "731")}));
        set("/fapi/v1/klines:BTCUSDT", Json::array({kline(JAN2_MS), nextOpen()}));
        set("/fapi/v1/fundingRate", Json::array());

        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("Cannot create fake Binance socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 128) != 0) {
            ::close(listener_);
            throw std::runtime_error("Cannot bind fake Binance loopback listener");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("Cannot read fake Binance port");
        }
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        try {
            worker_ = std::thread([this] { serve(); });
        } catch (...) {
            ::close(listener_);
            throw;
        }
    }

    ~FakeBinanceHttp()
    {
        stopping_ = true;
        ::shutdown(listener_, SHUT_RDWR);
        if (worker_.joinable())
            worker_.join();
        ::close(listener_);
    }

    FakeBinanceHttp(const FakeBinanceHttp&) = delete;
    FakeBinanceHttp& operator=(const FakeBinanceHttp&) = delete;

    const std::string& url() const { return url_; }

    void set(const std::string& route, const Json& body)
    {
        script(route, {body.dump()});
    }

    void script(const std::string& route, std::initializer_list<std::string> bodies)
    {
        if (bodies.size() == 0)
            throw std::invalid_argument("Fake Binance route needs a response");
        const std::lock_guard lock(mutex_);
        responses_[route] = std::deque<std::string>(bodies);
    }

    std::vector<Request> requests() const
    {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

    void clearRequests()
    {
        const std::lock_guard lock(mutex_);
        requests_.clear();
    }

private:
    int listener_ = -1;
    std::string url_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::map<std::string, std::deque<std::string>> responses_;
    std::vector<Request> requests_;

    static Request parse(const std::string& target)
    {
        Request request;
        const auto question = target.find('?');
        request.path = target.substr(0, question);
        if (question == std::string::npos)
            return request;
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
        return request;
    }

    void answer(int client)
    {
        const timeval timeout{2, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string header;
        char buffer[4096];
        while (header.find("\r\n\r\n") == std::string::npos && header.size() < 65536) {
            const auto received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0)
                return;
            header.append(buffer, static_cast<std::size_t>(received));
        }
        const auto firstSpace = header.find(' ');
        const auto secondSpace = header.find(' ', firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
            return;
        const Request request = parse(header.substr(firstSpace + 1, secondSpace - firstSpace - 1));
        std::string body;
        bool found = false;
        {
            const std::lock_guard lock(mutex_);
            requests_.push_back(request);
            auto response = responses_.end();
            if (const auto item = request.query.find("symbol"); item != request.query.end())
                response = responses_.find(request.path + ":" + item->second);
            if (response == responses_.end())
                response = responses_.find(request.path);
            if (response != responses_.end()) {
                found = true;
                body = response->second.front();
                if (response->second.size() > 1)
                    response->second.pop_front();
            }
        }
        // Missing routes fail immediately with a non-retryable HTTP status.
        const std::string wire = std::string(found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") +
            "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < wire.size()) {
            const auto count = ::send(client, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
            if (count <= 0)
                return;
            sent += static_cast<std::size_t>(count);
        }
    }

    void serve()
    {
        while (!stopping_) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            answer(client);
            ::close(client);
        }
    }
};

} // namespace market_data_test
