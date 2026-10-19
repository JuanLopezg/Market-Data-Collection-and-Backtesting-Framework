// Fake Kraken Futures venue for the exchange-gateway tests (tests/specs/exchange_gateway.md).
//
// Recorded Kraken-shaped answers per endpoint; every request is kept for inspection.
// Nothing leaves the process and the secret is synthetic (bytes 0..63).

#pragma once

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "venue/http_transport.h"
#include "venue/kraken_futures_protocol.h"
#include "venue/live_venue_adapter.h"
#include "venue/symbol_map.h"
#include "venue/venue_journal.h"

namespace venue_fakes {

using namespace Venue;

// Synthetic secret: Base64 of the bytes 0..63.
inline const std::string SECRET =
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+Pw==";
inline const KrakenCredentials CREDENTIALS{"testkey12345", SECRET};
inline const std::string HOST = "https://futures.kraken.com";
inline const std::string NAMESPACE = "t1";

inline constexpr Timestamp TODAY = 20261012;
inline constexpr long long T0_MS = 1791763205000LL; // 2026-10-12T00:00:05Z

inline HttpResponse ok(const std::string& body) { return {true, 200, body, {}}; }
inline HttpResponse httpStatus(long status, const std::string& body = "{}") { return {true, status, body, {}}; }
inline HttpResponse noAnswer() { return {false, 0, {}, "timed out"}; }

inline const std::string INSTRUMENTS = R"({"result":"success","instruments":[
 {"symbol":"PF_XBTUSD","type":"flexible_futures","base":"XBT","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":4,"tickSize":1,"tradfi":false},
 {"symbol":"PF_ETHUSD","type":"flexible_futures","base":"ETH","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":3,"tickSize":0.1,"tradfi":false},
 {"symbol":"PF_PEPEUSD","type":"flexible_futures","base":"PEPE","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.0000000001,"tradfi":false},
 {"symbol":"PF_OLDUSD","type":"flexible_futures","base":"OLD","quote":"USD","tradeable":true,"isExpired":true,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01,"tradfi":false},
 {"symbol":"PF_NOPXUSD","type":"flexible_futures","base":"NOPX","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01,"tradfi":false}]})";

inline const std::string TICKERS = R"({"result":"success","tickers":[
 {"symbol":"PF_XBTUSD","markPrice":62000.0,"suspended":false},
 {"symbol":"PF_ETHUSD","markPrice":2500.0,"suspended":false},
 {"symbol":"PF_PEPEUSD","markPrice":0.00001,"suspended":false},
 {"symbol":"PF_OLDUSD","markPrice":1.0,"suspended":false}]})";

inline const std::string EMPTY_ACCOUNT_LOG = R"({"logs":[]})";

inline std::string placed(const std::string& venueOrderId = "venue-order-1")
{
    return R"({"result":"success","sendStatus":{"order_id":")" + venueOrderId + R"(","status":"placed"}})";
}

inline std::string sendStatus(const std::string& status)
{
    return R"({"result":"success","sendStatus":{"status":")" + status + R"("}})";
}

inline std::string venueError(const std::string& code)
{
    return R"({"result":"error","error":")" + code + R"("})";
}

inline std::string fillsBody(const std::vector<std::string>& rows)
{
    std::string joined;
    for (const std::string& row : rows)
        joined += (joined.empty() ? "" : ",") + row;
    return R"({"result":"success","fills":[)" + joined + "]}";
}

inline std::string fillRow(const std::string& fillId, const std::string& clientId, const std::string& symbol,
                           const std::string& side, const std::string& size, const std::string& price,
                           const std::string& time = "2026-10-12T00:00:07.250Z",
                           const std::string& venueOrderId = "venue-order-1")
{
    const std::string client = clientId.empty() ? "null" : "\"" + clientId + "\"";
    return R"({"fill_id":")" + fillId + R"(","order_id":")" + venueOrderId + R"(","cliOrdId":)" + client +
           R"(,"symbol":")" + symbol + R"(","side":")" + side + R"(","size":)" + size + R"(,"price":)" + price +
           R"(,"fillTime":")" + time + R"(","fillType":"taker"})";
}

inline std::string openOrdersBody(const std::vector<std::string>& rows)
{
    std::string joined;
    for (const std::string& row : rows)
        joined += (joined.empty() ? "" : ",") + row;
    return R"({"result":"success","openOrders":[)" + joined + "]}";
}

inline std::string openOrderRow(const std::string& clientId, const std::string& symbol, const std::string& side,
                                const std::string& filled, const std::string& unfilled,
                                const std::string& venueOrderId = "venue-order-1")
{
    return R"({"order_id":")" + venueOrderId + R"(","cliOrdId":")" + clientId + R"(","symbol":")" + symbol +
           R"(","side":")" + side + R"(","filledSize":)" + filled + R"(,"unfilledSize":)" + unfilled + "}";
}

inline SymbolMap testSymbolMap(double pepeScale = 1000.0)
{
    return SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0},
                                        {"ETHUSDT", "ETH", "PF_ETHUSD", 1.0},
                                        {"1000PEPEUSDT", "PEPE", "PF_PEPEUSD", pepeScale},
                                        {"OLDUSDT", "OLD", "PF_OLDUSD", 1.0},
                                        {"NOPXUSDT", "NOPX", "PF_NOPXUSD", 1.0}});
}

// Queued answers per endpoint name (the last one repeats); records every request.
class ScriptedTransport final : public HttpTransport {
public:
    std::map<std::string, std::deque<HttpResponse>> responses;
    std::vector<HttpRequest> requests;
    std::function<void(const HttpRequest&)> on_send; // called before answering

    ScriptedTransport()
    {
        responses["instruments"] = {ok(INSTRUMENTS)};
        responses["tickers"] = {ok(TICKERS)};
        responses["openorders"] = {ok(openOrdersBody({}))};
        responses["fills"] = {ok(fillsBody({}))};
        responses["account-log"] = {ok(EMPTY_ACCOUNT_LOG)};
    }

    void script(const std::string& endpoint, std::vector<HttpResponse> answers)
    {
        responses[endpoint] = std::deque<HttpResponse>(answers.begin(), answers.end());
    }

    HttpResponse send(const HttpRequest& request) override
    {
        requests.push_back(request);
        if (on_send)
            on_send(request);
        auto& queue = responses[request.endpoint];
        if (queue.empty())
            return {false, 0, {}, "no scripted response for " + request.endpoint};
        HttpResponse response = queue.front();
        if (queue.size() > 1)
            queue.pop_front();
        return response;
    }

    int count(const std::string& endpoint) const
    {
        int result = 0;
        for (const HttpRequest& request : requests)
            result += request.endpoint == endpoint ? 1 : 0;
        return result;
    }

    int posts() const
    {
        int result = 0;
        for (const HttpRequest& request : requests)
            result += request.method == "POST" ? 1 : 0;
        return result;
    }

    std::vector<HttpRequest> requestsTo(const std::string& endpoint) const
    {
        std::vector<HttpRequest> result;
        for (const HttpRequest& request : requests)
            if (request.endpoint == endpoint)
                result.push_back(request);
        return result;
    }
};

struct Events {
    std::vector<OrderUpdateEvent> updates;
    std::vector<FillEvent> fills;
    std::vector<ExchangeSnapshotEvent> snapshots;
    std::vector<VenueCostEvent> costs;
    std::vector<std::string> sequence; // "fill:<order>" / "update:<order>:<status>"

    ExchangeGatewayHandlers handlers()
    {
        return {[this](const OrderUpdateEvent& e) {
                    updates.push_back(e);
                    sequence.push_back("update:" + std::to_string(e.update.order_id) + ":" +
                                       executionOrderStatusName(e.update.status));
                },
                [this](const FillEvent& e) {
                    fills.push_back(e);
                    sequence.push_back("fill:" + std::to_string(e.fill.order_id));
                },
                [this](const ExchangeSnapshotEvent& e) { snapshots.push_back(e); },
                [this](const VenueCostEvent& e) { costs.push_back(e); }};
    }

    std::vector<ExecutionOrderStatus> statusesOf(OrderID id) const
    {
        std::vector<ExecutionOrderStatus> result;
        for (const OrderUpdateEvent& event : updates)
            if (event.update.order_id == id)
                result.push_back(event.update.status);
        return result;
    }
};

// Value of key=value in a form body ("" when absent).
inline std::string formValue(const std::string& body, const std::string& key)
{
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t end = body.find('&', start);
        const std::string pair = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (pair.rfind(key + "=", 0) == 0)
            return pair.substr(key.size() + 1);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {};
}

inline bool formHas(const std::string& body, const std::string& key)
{
    return ("&" + body).find("&" + key + "=") != std::string::npos;
}

inline std::string header(const HttpRequest& request, const std::string& name)
{
    for (const auto& [key, value] : request.headers)
        if (key == name)
            return value;
    return {};
}

inline bool hasHeader(const HttpRequest& request, const std::string& name)
{
    for (const auto& [key, value] : request.headers)
        if (key == name)
            return true;
    return false;
}

} // namespace venue_fakes
