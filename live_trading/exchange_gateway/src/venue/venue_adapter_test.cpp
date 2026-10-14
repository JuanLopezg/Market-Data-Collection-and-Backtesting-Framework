// Offline tests for the real-venue adapter and the Kraken Futures protocol.
//
// Every response is recorded in this file (shapes from the Kraken Futures REST reference);
// no request leaves the process. Run: build/live_trading/exchange_gateway/src/algotrading_venue_adapter_test

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "venue/http_transport.h"
#include "venue/kraken_futures_protocol.h"
#include "venue/live_venue_adapter.h"
#include "venue/venue_journal.h"

using namespace Venue;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(condition)) {                                                              \
            ++failures;                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
        }                                                                                \
    } while (false)

// Synthetic secret (bytes 0..63); reference signatures computed with Python hashlib/hmac.
const std::string SECRET = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+Pw==";
const KrakenCredentials CREDENTIALS{"testkey12345", SECRET};
const Timestamp TODAY = 20261012;

HttpResponse ok(const std::string& body) { return {true, 200, body, {}}; }

const std::string INSTRUMENTS = R"({"result":"success","serverTime":"2026-10-12T00:00:01.000Z","instruments":[
 {"symbol":"PF_XBTUSD","type":"flexible_futures","base":"XBT","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":4,"tickSize":1,"tradfi":false},
 {"symbol":"PF_ETHUSD","type":"flexible_futures","base":"ETH","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":3,"tickSize":0.1,"tradfi":false},
 {"symbol":"PF_PEPEUSD","type":"flexible_futures","base":"PEPE","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.0000000001,"tradfi":false},
 {"symbol":"PF_OLDUSD","type":"flexible_futures","base":"OLD","quote":"USD","tradeable":true,"isExpired":true,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01,"tradfi":false},
 {"symbol":"PF_AAPLXUSD","type":"flexible_futures","base":"AAPLX","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":2,"tickSize":0.01,"tradfi":true},
 {"symbol":"FI_XBTUSD_261225","type":"futures_inverse","base":"XBT","quote":"USD","tradeable":true,"isExpired":false,
  "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.5}]})";

const std::string TICKERS = R"({"result":"success","serverTime":"2026-10-12T00:00:01.000Z","tickers":[
 {"symbol":"PF_XBTUSD","markPrice":62000.0,"suspended":false,"tag":"perpetual"},
 {"symbol":"PF_ETHUSD","markPrice":2500.0,"suspended":false,"tag":"perpetual"},
 {"symbol":"PF_PEPEUSD","markPrice":0.00001,"suspended":false,"tag":"perpetual"},
 {"symbol":"PF_HALTUSD","markPrice":3.0,"suspended":true,"tag":"perpetual"},
 {"symbol":"FI_XBTUSD_261225","markPrice":62500.0,"suspended":false,"tag":"month"}]})";

// Planning prices in source units: 1000PEPEUSDT is 1000 PEPE, so 1000 x the PEPE price.
const std::map<Coin, double> REFERENCE_PRICES = {
    {"BTCUSDT", 62000.0}, {"ETHUSDT", 2500.0}, {"1000PEPEUSDT", 0.01}, {"OLDUSDT", 1.0}, {"XYZUSDT", 1.0}};

SymbolMap testSymbolMap(double pepeScale = 1000.0)
{
    return SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0},
                                        {"ETHUSDT", "ETH", "PF_ETHUSD", 1.0},
                                        {"1000PEPEUSDT", "PEPE", "PF_PEPEUSD", pepeScale},
                                        {"OLDUSDT", "OLD", "PF_OLDUSD", 1.0}});
}

const std::string PLACED =R"({"result":"success","serverTime":"2026-10-12T00:00:06.000Z",
 "sendStatus":{"order_id":"11111111-1111-1111-1111-111111111111","status":"placed","receivedTime":"2026-10-12T00:00:06.000Z",
 "orderEvents":[],"cliOrdId":"algo-t1-7"}})";

std::string fills(const std::string& rows) { return R"({"result":"success","serverTime":"2026-10-12T00:00:09.000Z","fills":[)" + rows + "]}"; }
std::string openOrders(const std::string& rows) { return R"({"result":"success","serverTime":"2026-10-12T00:00:09.000Z","openOrders":[)" + rows + "]}"; }

std::string fill(const std::string& id, const std::string& client, const std::string& symbol, const std::string& side,
                 const std::string& size, const std::string& price, const std::string& time = "2026-10-12T00:00:07.250Z")
{
    const std::string clientField = client.empty() ? "null" : "\"" + client + "\"";
    return R"({"fill_id":")" + id + R"(","symbol":")" + symbol + R"(","side":")" + side +
           R"(","order_id":"11111111-1111-1111-1111-111111111111","cliOrdId":)" + clientField + R"(,"size":)" + size +
           R"(,"price":)" + price + R"(,"fillTime":")" + time + R"(","fillType":"taker"})";
}

// Returns queued responses per endpoint (the last one repeats) and records every request.
class ScriptedTransport final : public HttpTransport {
public:
    std::map<std::string, std::deque<HttpResponse>> responses;
    std::vector<HttpRequest> requests;

    HttpResponse send(const HttpRequest& request) override
    {
        requests.push_back(request);
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
};

struct Events {
    std::vector<OrderUpdateEvent> updates;
    std::vector<FillEvent> fills;
    std::vector<ExchangeSnapshotEvent> snapshots;

    ExchangeGatewayHandlers handlers()
    {
        return {[this](const OrderUpdateEvent& e) { updates.push_back(e); },
                [this](const FillEvent& e) { fills.push_back(e); },
                [this](const ExchangeSnapshotEvent& e) { snapshots.push_back(e); }};
    }
};

// One adapter over a fresh (or reused) journal, with test-controlled clocks.
struct Harness {
    std::string journal_path;
    SymbolMap symbol_map;
    ScriptedTransport transport;
    KrakenFuturesProtocol protocol{"https://futures.kraken.com", CREDENTIALS};
    long long now = 1791763200000; // 2026-10-12 00:00:00 UTC
    Timestamp today = TODAY;
    Events events;
    std::unique_ptr<VenueJournal> journal;
    std::unique_ptr<LiveVenueAdapter> adapter;

    explicit Harness(const std::string& name, bool enabled = true, bool fresh = true, SymbolMap map = testSymbolMap())
        : journal_path((std::filesystem::temp_directory_path() / ("venue-test-" + name + ".sqlite")).string()),
          symbol_map(std::move(map))
    {
        if (fresh)
            removeJournal();
        transport.responses["instruments"] = {ok(INSTRUMENTS)};
        transport.responses["tickers"] = {ok(TICKERS)};
        open(enabled);
    }

    void removeJournal()
    {
        for (const char* suffix : {"", "-wal", "-shm"})
            std::filesystem::remove(journal_path + suffix);
    }

    void open(bool enabled)
    {
        adapter.reset();
        journal = std::make_unique<VenueJournal>(journal_path, "kraken-futures", "t1");
        LiveVenueOptions options;
        options.order_submission_enabled = enabled;
        options.account_namespace = "t1";
        adapter = std::make_unique<LiveVenueAdapter>(protocol, transport, *journal, symbol_map,
            [this] { return today; }, [this] { return now; }, options);
        adapter->setHandlers(events.handlers());
    }

    // referencePrice < 0: the planning price from REFERENCE_PRICES.
    void submit(OrderID id, const Coin& coin, OrderSide side, double quantity, Timestamp activeFrom = TODAY,
                double referencePrice = -1.0)
    {
        SubmitOrderCommand command;
        command.metadata.message_id = "submit-" + std::to_string(id);
        command.order = ExecutionOrder(id, 1, activeFrom - 1, activeFrom, coin, side, quantity);
        command.order.reference_price = referencePrice >= 0.0 ? referencePrice : REFERENCE_PRICES.at(coin);
        adapter->submitOrder(command);
    }

    void poll(long long advanceMs = 3000)
    {
        now += advanceMs;
        adapter->poll(50);
    }

    std::string bodyOf(const std::string& endpoint) const
    {
        for (const HttpRequest& request : transport.requests)
            if (request.endpoint == endpoint)
                return request.body;
        return {};
    }
};

bool near(double a, double b) { return std::abs(a - b) < 1e-9 * std::max(1.0, std::abs(b)); }

// ------------------------------------------------------------------ tests

void testSigningMatchesReference()
{
    CHECK(KrakenFuturesProtocol::authent(SECRET, "", "", "/api/v3/accounts") ==
          "QBye7cezp0pKjoos3UxEPd8Y4zfzoNG4k2coPuQvof0HlKBQn7RFH8fqt7jPNYVUD32XTYNtlcH/m28djrR8KA==");
    CHECK(KrakenFuturesProtocol::authent(SECRET, "orderType=mkt&symbol=PF_XBTUSD&side=buy&size=0.0012&cliOrdId=algo-t1-7",
                                         "1791700000000", "/api/v3/sendorder") ==
          "elFZ7IKtspvuaVUTLkHI5MtrJYvJIiBDpJ6As/2AEHbx7aXX1JonDaP3/DYCX6Y4S1BdCOUosCl/J2bsxpYVmA==");

    KrakenFuturesProtocol protocol("https://futures.kraken.com", CREDENTIALS);
    const HttpRequest get = protocol.accountsRequest();
    CHECK(get.method == "GET" && get.url == "https://futures.kraken.com/derivatives/api/v3/accounts");
    bool hasNonce = false;
    for (const auto& header : get.headers)
        hasNonce = hasNonce || header.first == "Nonce";
    CHECK(!hasNonce);
}

void testCredentialFile()
{
    const auto path = std::filesystem::temp_directory_path() / "venue-test-credentials.env";
    std::ofstream(path) << "# comment\nKRAKEN_API_KEY=abcdefgh123\nKRAKEN_API_SECRET=\"" << SECRET << "\"\n";
    const KrakenCredentials loaded = loadKrakenCredentials(path.string());
    CHECK(loaded.api_key == "abcdefgh123" && loaded.api_secret == SECRET);

    std::ofstream(path) << "KRAKEN_API_KEY=abcdefgh123\nOTHER=value\n";
    bool refused = false;
    try { loadKrakenCredentials(path.string()); } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("abcdefgh123") == std::string::npos; // No contents in errors.
    }
    CHECK(refused);

    const auto link = std::filesystem::temp_directory_path() / "venue-test-credentials-link.env";
    std::filesystem::remove(link);
    std::filesystem::create_symlink(path, link);
    refused = false;
    try { loadKrakenCredentials(link.string()); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
    std::filesystem::remove(link);
    std::filesystem::remove(path);
}

void testMarketCatalogue()
{
    KrakenFuturesProtocol protocol("https://futures.kraken.com", CREDENTIALS);
    const auto markets = protocol.parseMarkets(ok(INSTRUMENTS));
    std::map<std::string, Market> byAsset;
    for (const Market& market : markets)
        byAsset[market.asset] = market;
    CHECK(markets.size() == 4); // tradfi and inverse futures excluded
    CHECK(byAsset["BTC"].symbol == "PF_XBTUSD" && byAsset["BTC"].trading && near(byAsset["BTC"].quantity_step, 0.0001));
    CHECK(byAsset["PEPE"].trading && near(byAsset["PEPE"].quantity_step, 1.0));
    CHECK(!byAsset["OLD"].trading && byAsset["OLD"].unavailable_reason == "expired");

    const auto marks = protocol.parseMarkPrices(ok(TICKERS));
    CHECK(marks.size() == 3); // suspended and non-PF rows left out
    CHECK(near(marks.at("PF_XBTUSD"), 62000.0) && near(marks.at("PF_PEPEUSD"), 0.00001));
}

void testSymbolMapFile()
{
    const auto path = std::filesystem::temp_directory_path() / "venue-test-symbol-map.json";
    const auto write = [&](const std::string& venue, const std::string& symbols) {
        std::ofstream(path) << R"({"schema_version":1,"venue":")" << venue << R"(","symbols":[)" << symbols << "]}";
    };
    const std::string btc = R"({"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1})";
    const std::string pepe = R"({"source":"1000PEPEUSDT","asset":"PEPE","venue_symbol":"PF_PEPEUSD","venue_units_per_source_unit":1000})";

    write("kraken-futures", btc + "," + pepe);
    const SymbolMap map = SymbolMap::load(path.string(), "kraken-futures");
    CHECK(map.entries().size() == 2);
    CHECK(map.bySource("1000PEPEUSDT") != nullptr && near(map.bySource("1000PEPEUSDT")->venue_units_per_source_unit, 1000));
    CHECK(map.byVenueSymbol("PF_XBTUSD") != nullptr && map.byVenueSymbol("PF_XBTUSD")->source == "BTCUSDT");
    CHECK(map.bySource("PEPEUSDT") == nullptr); // no guessing from names

    const auto refused = [&](const std::string& venue, const std::string& symbols) {
        write(venue, symbols);
        try { SymbolMap::load(path.string(), "kraken-futures"); } catch (const std::exception&) { return true; }
        return false;
    };
    CHECK(refused("hyperliquid", btc));                                  // another venue's table
    CHECK(refused("kraken-futures", btc + "," + btc));                   // source listed twice
    CHECK(refused("kraken-futures", btc + R"(,{"source":"XBTUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1})"));
    CHECK(refused("kraken-futures", R"({"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":0})"));
    CHECK(refused("kraken-futures", R"({"source":"BTCUSDT","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1})"));
    std::filesystem::remove(path);
}

void testNotionalCheckCatchesWrongScale()
{
    // A map that forgot the x1000 of 1000PEPEUSDT: 5 units would become 5 PEPE ($0.00005)
    // instead of 5000 PEPE ($0.05). The order never reaches the venue.
    Harness h("wrong-scale", true, true, testSymbolMap(1.0));
    h.submit(8, "1000PEPEUSDT", OrderSide::Sell, 5.0);
    h.submit(9, "BTCUSDT", OrderSide::Buy, 0.0012, TODAY, 0.0); // no planning price
    h.poll();
    CHECK(h.transport.count("sendorder") == 0);
    CHECK(h.events.updates.size() == 2);
    if (h.events.updates.size() == 2) {
        CHECK(h.events.updates[0].update.message.rfind("NOTIONAL_MISMATCH", 0) == 0);
        CHECK(h.events.updates[1].update.message.rfind("NO_REFERENCE_PRICE", 0) == 0);
    }

    // The correct scale passes, and a normal price move (+20%) is not a mismatch.
    Harness right("right-scale");
    right.transport.responses["sendorder"] = {ok(PLACED)};
    right.submit(8, "1000PEPEUSDT", OrderSide::Sell, 5.0, TODAY, 0.012);
    right.poll();
    CHECK(right.transport.count("sendorder") == 1);
}

void testDisabledSubmissionNeverPosts()
{
    Harness h("disabled", false);
    GetOnlyTransport guard(h.transport);
    bool refused = false;
    try { guard.send({"POST", "https://futures.kraken.com/derivatives/api/v3/sendorder", {}, "x", "sendorder"}); }
    catch (const std::logic_error&) { refused = true; }
    CHECK(refused);
    CHECK(h.transport.requests.empty());

    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.00123);
    h.poll();
    CHECK(h.transport.count("sendorder") == 0);
    CHECK(h.events.updates.size() == 1 && h.events.updates[0].update.status == ExecutionOrderStatus::Rejected);
    CHECK(h.events.updates[0].update.message.find("submission disabled") != std::string::npos);
}

void testAcceptedFilledAndRestartWithoutDuplicates()
{
    Harness h("happy");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills(fill("f-1", "algo-t1-7", "PF_XBTUSD", "buy", "0.0012", "62000.5")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};

    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.00123);
    h.poll();
    CHECK(h.transport.count("sendorder") == 1);
    CHECK(h.bodyOf("sendorder") == "orderType=mkt&symbol=PF_XBTUSD&side=buy&size=0.0012&cliOrdId=algo-t1-7");
    h.poll();
    CHECK(h.events.fills.size() == 1);
    if (!h.events.fills.empty()) {
        const Fill& f = h.events.fills[0].fill;
        CHECK(f.fill_id == 1 && f.order_id == 7 && f.coin == "BTCUSDT" && near(f.quantity, 0.0012) && near(f.price, 62000.5));
        CHECK(f.timestamp == TODAY && h.events.fills[0].metadata.message_id == "kraken-futures-fill:f-1");
    }
    CHECK(h.events.updates.size() == 2);
    if (h.events.updates.size() == 2) {
        CHECK(h.events.updates[0].update.status == ExecutionOrderStatus::Accepted);
        CHECK(h.events.updates[1].update.status == ExecutionOrderStatus::Filled);
    }

    // Restart on the same journal: same fills are seen again but nothing is re-published or resent.
    Events before = h.events;
    h.open(true);
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.00123); // NATS redelivery of the same command
    h.poll();
    h.poll();
    CHECK(h.transport.count("sendorder") == 1);
    CHECK(h.events.fills.size() == before.fills.size() && h.events.updates.size() == before.updates.size());
}

void testScaledSourceCoin()
{
    Harness h("scaled");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills(fill("f-p", "algo-t1-8", "PF_PEPEUSD", "sell", "5000", "0.00001")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(8, "1000PEPEUSDT", OrderSide::Sell, 5.0);
    h.poll();
    CHECK(h.bodyOf("sendorder").find("symbol=PF_PEPEUSD&side=sell&size=5000&") != std::string::npos);
    h.poll();
    CHECK(h.events.fills.size() == 1);
    if (!h.events.fills.empty()) // Back to source units: 5 x 1000PEPE at 0.01 USD.
        CHECK(near(h.events.fills[0].fill.quantity, 5.0) && near(h.events.fills[0].fill.price, 0.01));
}

void testLocalRejectionsDoNotReachVenue()
{
    Harness h("local-rejections");
    h.submit(9, "XYZUSDT", OrderSide::Buy, 1.0);     // not in the symbol map
    h.submit(10, "BTCUSDT", OrderSide::Buy, 0.00001); // below 0.0001 step
    h.submit(11, "OLDUSDT", OrderSide::Buy, 1.0);     // expired
    h.poll();
    CHECK(h.transport.count("sendorder") == 0);
    CHECK(h.events.updates.size() == 3);
    for (const auto& event : h.events.updates)
        CHECK(event.update.status == ExecutionOrderStatus::Rejected);
    if (h.events.updates.size() == 3) {
        CHECK(h.events.updates[0].update.message.rfind("MARKET_UNAVAILABLE", 0) == 0);
        CHECK(h.events.updates[1].update.message.rfind("BELOW_MINIMUM_QUANTITY", 0) == 0);
        CHECK(h.events.updates[2].update.message.rfind("MARKET_NOT_TRADING", 0) == 0);
    }
}

void testAmbiguousSubmitIsResolvedByEvidenceNotResent()
{
    Harness h("ambiguous");
    h.transport.responses["sendorder"] = {{false, 0, {}, "HTTP request to sendorder timed out"}};
    h.transport.responses["fills"] = {ok(fills("")), ok(fills(fill("f-2", "algo-t1-7", "PF_XBTUSD", "buy", "0.0012", "62001")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
    h.poll();
    CHECK(h.journal->order(7)->state == OrderState::Unknown);
    h.poll();
    h.poll();
    CHECK(h.transport.count("sendorder") == 1); // never resent
    CHECK(h.journal->order(7)->state == OrderState::Filled);
    CHECK(h.events.fills.size() == 1);
}

void testCrashWhileSendingBecomesUnknown()
{
    {
        Harness h("crash");
        h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
        JournalOrder order = *h.journal->order(7);
        order.state = OrderState::Sending; // as if the process stopped mid-request
        order.sent_at_ms = h.now;
        h.journal->updateOrder(order);
    }
    Harness h("crash", true, false);
    h.transport.responses["fills"] = {ok(fills(""))};
    h.transport.responses["openorders"] = {ok(openOrders(R"({"order_id":"11111111-1111-1111-1111-111111111111",
        "cliOrdId":"algo-t1-7","status":"untouched","side":"buy","orderType":"lmt","symbol":"PF_XBTUSD",
        "filledSize":0,"unfilledSize":0.0012,"reduceOnly":false,"receivedTime":"2026-10-12T00:00:06.000Z",
        "lastUpdateTime":"2026-10-12T00:00:06.000Z"})"))};
    h.poll();
    CHECK(h.transport.count("sendorder") == 0);
    CHECK(h.journal->order(7)->state == OrderState::Open); // venue lists it
}

void testNotProcessedIsSentAgain()
{
    Harness h("not-processed");
    h.transport.responses["sendorder"] = {
        ok(R"({"result":"error","error":"apiLimitExceeded","serverTime":"2026-10-12T00:00:06.000Z"})"), ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills(""))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
    h.poll();
    CHECK(h.journal->order(7)->state == OrderState::Prepared);
    h.poll();
    CHECK(h.transport.count("sendorder") == 2 && h.journal->order(7)->state == OrderState::Open);
}

void testHeldUntilActiveDayAndCancelBeforeSend()
{
    Harness h("held");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012, TODAY + 1);
    h.submit(12, "ETHUSDT", OrderSide::Buy, 0.5, TODAY + 1);
    h.poll();
    CHECK(h.transport.count("sendorder") == 0);

    CancelOrderCommand cancel;
    cancel.metadata.message_id = "cancel-12";
    cancel.order_id = 12;
    cancel.requested_at = TODAY;
    h.adapter->cancelOrder(cancel);
    CHECK(h.journal->order(12)->state == OrderState::Cancelled);
    CHECK(h.transport.count("cancelorder") == 0);

    h.today = TODAY + 1;
    h.poll();
    CHECK(h.transport.count("sendorder") == 1);
}

void testCancelOpenOrder()
{
    Harness h("cancel");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["cancelorder"] = {ok(R"({"result":"success","serverTime":"2026-10-12T00:00:08.000Z",
        "cancelStatus":{"status":"cancelled","order_id":"11111111-1111-1111-1111-111111111111","receivedTime":"2026-10-12T00:00:08.000Z","orderEvents":[]}})")};
    h.transport.responses["fills"] = {ok(fills(""))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
    h.poll(1);

    CancelOrderCommand cancel;
    cancel.metadata.message_id = "cancel-7";
    cancel.order_id = 7;
    h.adapter->cancelOrder(cancel);
    h.poll(1);
    CHECK(h.bodyOf("cancelorder") == "cliOrdId=algo-t1-7");
    CHECK(h.journal->order(7)->state == OrderState::Cancelled);
    CHECK(!h.events.updates.empty() && h.events.updates.back().update.status == ExecutionOrderStatus::Canceled);
}

void testPartialFillThenRemainderEnds()
{
    Harness h("partial");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills(fill("f-3", "algo-t1-7", "PF_XBTUSD", "buy", "0.0005", "62000")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
    h.poll();
    h.poll();
    CHECK(h.journal->order(7)->state == OrderState::Open);
    h.poll(20000);
    CHECK(h.journal->order(7)->state == OrderState::Cancelled);
    CHECK(near(h.journal->order(7)->filled_venue_quantity, 0.0005));
    CHECK(h.events.fills.size() == 1);
}

void testUnattributedAndReusedClientIdFills()
{
    Harness h("unattributed");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills(
        fill("f-liq", "", "PF_ETHUSD", "sell", "0.1", "2500") + "," +
        fill("f-old", "algo-t1-7", "PF_XBTUSD", "buy", "0.0012", "50000", "2026-09-01T00:00:00.000Z")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.0012);
    h.poll();
    h.poll();
    CHECK(h.events.fills.empty());
    CHECK(h.journal->knowsFill("f-liq") && h.journal->knowsFill("f-old"));
}

void testSnapshot()
{
    Harness h("snapshot", false);
    h.transport.responses["accounts"] = {ok(R"({"result":"success","serverTime":"2026-10-12T00:00:01.000Z","accounts":{
        "flex":{"type":"multiCollateralMarginAccount","currencies":{"USD":{"quantity":120.5,"value":120.5,"collateral":120.5,"available":100},
        "XBT":{"quantity":0.01,"value":620,"collateral":613.8,"available":0.01}}}}})")};
    h.transport.responses["openpositions"] = {ok(R"({"result":"success","serverTime":"2026-10-12T00:00:01.000Z","openPositions":[
        {"side":"long","symbol":"PF_XBTUSD","price":60000,"fillTime":"2026-10-11T00:00:07.000Z","size":0.002,"unrealizedFunding":0},
        {"side":"short","symbol":"PF_PEPEUSD","price":0.00001,"fillTime":"2026-10-11T00:00:07.000Z","size":7000,"unrealizedFunding":0}]})")};
    h.transport.responses["openorders"] = {ok(openOrders(""))};

    ExchangeSnapshotRequest request;
    request.metadata.message_id = "snapshot-1";
    h.adapter->requestSnapshot(request);
    CHECK(h.events.snapshots.size() == 1);
    if (!h.events.snapshots.empty()) {
        const auto& event = h.events.snapshots[0];
        CHECK(event.metadata.message_id == "kraken-futures-snapshot:snapshot-1" && event.metadata.correlation_id == "snapshot-1");
        CHECK(near(event.snapshot.cash, 120.5) && event.snapshot.timestamp == TODAY);
        CHECK(near(event.snapshot.positions.at("BTCUSDT"), 0.002));
        CHECK(near(event.snapshot.positions.at("1000PEPEUSDT"), -7.0));
    }
    for (const HttpRequest& request : h.transport.requests)
        CHECK(request.method == "GET");
}

void testPermissions()
{
    const std::string readOnly = R"({"apiKey":"testkey12345","accountUid":"x","permissions":{"general":"READ_ONLY","transfer":"NO_ACCESS"}})";
    const std::string transfer = R"({"apiKey":"testkey12345","accountUid":"x","permissions":{"general":"FULL_ACCESS","transfer":"FULL_ACCESS"}})";

    Harness disabled("permissions-ro", false);
    disabled.transport.responses["permissions"] = {ok(readOnly)};
    const Permissions permissions = disabled.adapter->verifyPermissions();
    CHECK(permissions.read && !permissions.trade && !permissions.withdraw);

    Harness enabled("permissions-enabled", true);
    enabled.transport.responses["permissions"] = {ok(readOnly)};
    bool refused = false;
    try { enabled.adapter->verifyPermissions(); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused); // submission enabled with a key that cannot trade

    Harness funds("permissions-transfer", false);
    funds.transport.responses["permissions"] = {ok(transfer)};
    refused = false;
    try { funds.adapter->verifyPermissions(); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused); // keys that can move funds are refused
}

// Optional real check with a read-only key: GET requests only (GetOnlyTransport), no
// orders. Prints counts and parse results, never balances, identifiers or credentials.
int liveReadOnlyProbe(const std::string& credentialsFile, const std::string& symbolMapFile)
{
    KrakenFuturesProtocol protocol("https://futures.kraken.com", loadKrakenCredentials(credentialsFile));
    CurlHttpTransport http(10000, 4 * 1024 * 1024);
    GetOnlyTransport transport(http);
    const auto journalPath = std::filesystem::temp_directory_path() / "venue-live-read-only-probe.sqlite";
    for (const char* suffix : {"", "-wal", "-shm"})
        std::filesystem::remove(journalPath.string() + suffix);
    VenueJournal journal(journalPath.string(), protocol.name(), "probe");

    LiveVenueOptions options;
    options.account_namespace = "probe";
    LiveVenueAdapter adapter(protocol, transport, journal, SymbolMap::load(symbolMapFile, protocol.name()),
        [] { return TODAY; },
        [] { return static_cast<long long>(std::time(nullptr)) * 1000; }, options);
    Events events;
    adapter.setHandlers(events.handlers());

    const Permissions permissions = adapter.verifyPermissions();
    ExchangeSnapshotRequest request;
    request.metadata.message_id = "read-only-probe";
    adapter.requestSnapshot(request);
    const auto fills = protocol.parseFills(transport.send(protocol.fillsRequest()));

    const auto& snapshot = events.snapshots.at(0).snapshot;
    std::cout << "LIVE-READ-ONLY-PROBE: permissions read=" << permissions.read << " trade=" << permissions.trade
              << " withdraw=" << permissions.withdraw << "; snapshot parsed: " << snapshot.positions.size()
              << " positions, " << snapshot.open_orders.size() << " open orders; fills parsed: " << fills.size()
              << "; requests: GET only\n";
    return 0;
}

} // namespace


int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--live-read-only") {
        try {
            return liveReadOnlyProbe(argv[2], argv[3]);
        }
        catch (const std::exception& error) {
            std::cerr << "LIVE-READ-ONLY-PROBE: FAILED: " << error.what() << "\n";
            return 1;
        }
    }

    // Quiet adapter logs; failures are printed by CHECK.
    setenv("ALGOTRADING_LOG_QUIET", "1", 1);

    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"signing matches the reference implementation", testSigningMatchesReference},
        {"credential file parsing", testCredentialFile},
        {"market catalogue and mark prices", testMarketCatalogue},
        {"symbol map file", testSymbolMapFile},
        {"notional check catches a wrong scale", testNotionalCheckCatchesWrongScale},
        {"disabled submission never posts", testDisabledSubmissionNeverPosts},
        {"accepted, filled and restart without duplicates", testAcceptedFilledAndRestartWithoutDuplicates},
        {"scaled source coin", testScaledSourceCoin},
        {"local rejections do not reach the venue", testLocalRejectionsDoNotReachVenue},
        {"ambiguous submit resolved by evidence, not resent", testAmbiguousSubmitIsResolvedByEvidenceNotResent},
        {"crash while sending becomes unknown", testCrashWhileSendingBecomesUnknown},
        {"not processed is sent again", testNotProcessedIsSentAgain},
        {"held until active day; cancel before send", testHeldUntilActiveDayAndCancelBeforeSend},
        {"cancel open order", testCancelOpenOrder},
        {"partial fill then remainder ends", testPartialFillThenRemainderEnds},
        {"unattributed and reused client id fills", testUnattributedAndReusedClientIdFills},
        {"snapshot", testSnapshot},
        {"permissions", testPermissions},
    };
    for (const auto& [name, test] : tests) {
        const int before = failures;
        try {
            test();
        }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": exception " << error.what() << "\n";
        }
        std::cout << (failures == before ? "PASS " : "FAIL ") << name << "\n";
    }
    std::cout << (failures == 0 ? "VENUE-ADAPTER-TEST: PASS" : "VENUE-ADAPTER-TEST: FAIL") << " (" << checks
              << " checks, " << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
