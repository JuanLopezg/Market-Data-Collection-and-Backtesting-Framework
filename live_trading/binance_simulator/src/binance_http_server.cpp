#include "binance_http_server.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

namespace {

std::map<std::string, std::string> parseQuery(std::string target)
{
    std::map<std::string, std::string> result;

    auto question = target.find('?');
    if (question == std::string::npos)
        return result;

    std::string query = target.substr(question + 1);
    std::size_t pos = 0;

    while (pos <= query.size())
    {
        auto amp = query.find('&', pos);
        std::string part = query.substr(
            pos,
            amp == std::string::npos ? std::string::npos : amp - pos);

        auto equals = part.find('=');
        if (equals != std::string::npos)
            result[part.substr(0, equals)] = part.substr(equals + 1);

        if (amp == std::string::npos)
            break;

        pos = amp + 1;
    }

    return result;
}

std::string targetPath(const std::string& target)
{
    auto question = target.find('?');
    return target.substr(0, question);
}

http::response<http::string_body> jsonResponse(
    http::status status,
    unsigned version,
    const json& body)
{
    http::response<http::string_body> response{status, version};
    response.set(http::field::server, "algotrading-binance-simulator");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(false);
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

http::response<http::string_body> handleRequest(
    HistoricalMarketData& marketData,
    const http::request<http::string_body>& request)
{
    std::string target(request.target().data(), request.target().size());
    std::string path = targetPath(target);
    auto query = parseQuery(target);

    if (path == "/sim/v1/clock")
    {
        if (request.method() != http::verb::post)
        {
            return jsonResponse(
                http::status::method_not_allowed,
                request.version(),
                {{"code", -1003}, {"msg", "Simulation clock requires POST"}});
        }

        if (!query.contains("serverTime"))
        {
            return jsonResponse(
                http::status::bad_request,
                request.version(),
                {{"code", -1102}, {"msg", "serverTime is required"}});
        }

        try {
            const std::int64_t dayMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::days{1}).count();
            std::int64_t serverTime = std::stoll(query["serverTime"]);

            if (serverTime < 0 || serverTime % dayMs != 0)
                throw std::runtime_error("serverTime must be a non-negative UTC midnight in milliseconds");

            marketData.setClockTimeMs(serverTime);

            return jsonResponse(
                http::status::ok,
                request.version(),
                {
                    {"serverTime", marketData.clockTimeMs()},
                    {"activeInstruments", marketData.activeInstruments().size()}
                });
        }
        catch (const std::exception& e) {
            return jsonResponse(
                http::status::bad_request,
                request.version(),
                {{"code", -1100}, {"msg", e.what()}});
        }
    }

    if (request.method() != http::verb::get)
    {
        return jsonResponse(
            http::status::method_not_allowed,
            request.version(),
            {{"code", -1003}, {"msg", "Only GET is supported for Binance-compatible endpoints"}});
    }

    if (path == "/fapi/v1/time")
    {
        return jsonResponse(
            http::status::ok,
            request.version(),
            {{"serverTime", marketData.clockTimeMs()}});
    }

    if (path == "/fapi/v1/exchangeInfo")
    {
        json symbols = json::array();
        for (const auto& instrument : marketData.activeInstruments())
        {
            symbols.push_back({
                {"symbol", instrument.exchangeSymbol},
                {"contractType", "PERPETUAL"},
                {"status", "TRADING"},
                {"baseAsset", instrument.baseAsset},
                {"quoteAsset", "USDT"}
            });
        }

        return jsonResponse(
            http::status::ok,
            request.version(),
            {
                {"timezone", "UTC"},
                {"serverTime", marketData.clockTimeMs()},
                {"symbols", symbols}
            });
    }

    if (path == "/fapi/v1/klines")
    {
        if (!query.contains("symbol") || !query.contains("interval"))
        {
            return jsonResponse(
                http::status::bad_request,
                request.version(),
                {{"code", -1102}, {"msg", "symbol and interval are required"}});
        }

        if (query["interval"] != "1d")
        {
            return jsonResponse(
                http::status::bad_request,
                request.version(),
                {{"code", -1120}, {"msg", "Only interval=1d is supported"}});
        }

        try {
            std::int64_t startTime = query.contains("startTime")
                ? std::stoll(query["startTime"])
                : 0;
            std::int64_t endTime = query.contains("endTime")
                ? std::stoll(query["endTime"])
                : marketData.clockTimeMs();
            std::size_t limit = query.contains("limit")
                ? static_cast<std::size_t>(std::stoul(query["limit"]))
                : 500;

            limit = std::min<std::size_t>(limit, 1500);

            json body = json::array();
            for (const auto& row : marketData.klines(
                     query["symbol"], startTime, endTime, limit))
            {
                body.push_back(json::array({
                    row.openTimeMs,
                    row.open,
                    row.high,
                    row.low,
                    row.close,
                    row.volume,
                    row.closeTimeMs,
                    "0",
                    0,
                    "0",
                    "0",
                    "0"
                }));
            }

            return jsonResponse(http::status::ok, request.version(), body);
        }
        catch (const std::exception& e) {
            return jsonResponse(
                http::status::bad_request,
                request.version(),
                {{"code", -1100}, {"msg", e.what()}});
        }
    }

    return jsonResponse(
        http::status::not_found,
        request.version(),
        {{"code", -1002}, {"msg", "Endpoint not implemented by simulator"}});
}

void handleSession(tcp::socket socket, HistoricalMarketData& marketData)
{
    try {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);

        auto response = handleRequest(marketData, request);
        http::write(socket, response);

        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    }
    catch (const std::exception& e) {
        std::cerr << "HTTP session failed: " << e.what() << '\n';
    }
}

}

BinanceHttpServer::BinanceHttpServer(
    HistoricalMarketData& marketData,
    boost::asio::ip::address address,
    std::uint16_t port)
    : marketData_(marketData),
      address_(address),
      port_(port)
{
}

void BinanceHttpServer::run()
{
    asio::io_context ioContext{1};
    tcp::endpoint endpoint{address_, port_};
    tcp::acceptor acceptor{ioContext, endpoint};

    std::cout << "Binance simulator listening on "
              << address_.to_string() << ':' << port_ << '\n';

    for (;;)
    {
        tcp::socket socket{ioContext};
        acceptor.accept(socket);

        std::thread(
            handleSession,
            std::move(socket),
            std::ref(marketData_)).detach();
    }
}
