// Binance last prices (tests/specs/execution_state_service.md, Part D).
//
// No real network: a one-shot HTTP server on 127.0.0.1 stands in for Binance.

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "binance_prices.h"

namespace {

// Answers exactly one HTTP request with a fixed status and body, and records the request line.
class OneShotHttpServer {
public:
    OneShotHttpServer(int status, std::string body)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 1) != 0)
            throw std::runtime_error("bind/listen failed");
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);

        thread_ = std::thread([this, status, body = std::move(body)] {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0)
                return;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                    break;
                request.append(buffer, static_cast<std::size_t>(received));
            }
            request_line_ = request.substr(0, request.find("\r\n"));
            const std::string response = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\n"
                                         "Content-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t written = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (written <= 0)
                    break;
                sent += static_cast<std::size_t>(written);
            }
            ::close(client);
        });
    }

    ~OneShotHttpServer()
    {
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        if (thread_.joinable())
            thread_.join();
    }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string requestLine()
    {
        if (thread_.joinable())
            thread_.join();
        return request_line_;
    }

private:
    int listener_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::string request_line_;
};

// A loopback port with nothing listening.
int closedPort()
{
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    ::close(probe);
    return port;
}

} // namespace


TEST(BinancePrices, D1_BaseUrlIsRequired)
{
    EXPECT_THROW(BinanceLastPrices(""), std::invalid_argument);
    EXPECT_THROW(BinanceLastPrices("///"), std::invalid_argument);
}

TEST(BinancePrices, D1_RequestGoesToTickerPriceWithTrailingSlashIgnored)
{
    OneShotHttpServer server(200, "[]");
    const BinanceLastPrices prices(server.baseUrl() + "//");
    EXPECT_TRUE(prices.fetchAll().empty());
    EXPECT_EQ(server.requestLine(), "GET /fapi/v1/ticker/price HTTP/1.1");
}

TEST(BinancePrices, D2_PricesAndTimesAreParsed)
{
    OneShotHttpServer server(200, R"([
        {"symbol":"BTCUSDT","price":"62000.50","time":1760000000000},
        {"symbol":"1000PEPEUSDT","price":"0.01234","time":1760000000123},
        {"symbol":"ETHUSDT","price":"2500"}])");
    const auto prices = BinanceLastPrices(server.baseUrl()).fetchAll();
    ASSERT_EQ(prices.size(), 3u);
    EXPECT_DOUBLE_EQ(prices.at("BTCUSDT").price, 62000.50);
    EXPECT_EQ(prices.at("BTCUSDT").time_ms, 1760000000000LL);
    EXPECT_DOUBLE_EQ(prices.at("1000PEPEUSDT").price, 0.01234);
    EXPECT_EQ(prices.at("1000PEPEUSDT").time_ms, 1760000000123LL);
    EXPECT_DOUBLE_EQ(prices.at("ETHUSDT").price, 2500.0);
    EXPECT_EQ(prices.at("ETHUSDT").time_ms, 0);
}

TEST(BinancePrices, D2_NonPositivePricesAreLeftOut)
{
    OneShotHttpServer server(200, R"([
        {"symbol":"BTCUSDT","price":"62000","time":1},
        {"symbol":"DEADUSDT","price":"0","time":1},
        {"symbol":"NEGUSDT","price":"-1.5","time":1}])");
    const auto prices = BinanceLastPrices(server.baseUrl()).fetchAll();
    EXPECT_EQ(prices.size(), 1u);
    EXPECT_TRUE(prices.contains("BTCUSDT"));
    EXPECT_FALSE(prices.contains("DEADUSDT"));
    EXPECT_FALSE(prices.contains("NEGUSDT"));
}

TEST(BinancePrices, D3_HttpErrorThrowsRuntimeError)
{
    OneShotHttpServer server(500, R"({"code":-1,"msg":"boom"})");
    EXPECT_THROW(BinanceLastPrices(server.baseUrl()).fetchAll(), std::runtime_error);
}

TEST(BinancePrices, D3_AnswerThatIsNotAListThrowsRuntimeError)
{
    OneShotHttpServer server(200, R"({"symbol":"BTCUSDT","price":"62000"})");
    EXPECT_THROW(BinanceLastPrices(server.baseUrl()).fetchAll(), std::runtime_error);
}

TEST(BinancePrices, D3_AnswerThatIsNotJsonThrowsRuntimeError)
{
    OneShotHttpServer server(200, "<html>maintenance</html>");
    EXPECT_THROW(BinanceLastPrices(server.baseUrl()).fetchAll(), std::runtime_error);
}

TEST(BinancePrices, D3_NoConnectionThrowsRuntimeError)
{
    const BinanceLastPrices prices("http://127.0.0.1:" + std::to_string(closedPort()));
    EXPECT_THROW(prices.fetchAll(), std::runtime_error);
}
