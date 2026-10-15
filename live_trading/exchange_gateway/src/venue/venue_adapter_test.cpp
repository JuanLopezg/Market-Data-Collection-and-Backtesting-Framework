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
    std::vector<VenueCostEvent> costs;

    ExchangeGatewayHandlers handlers()
    {
        return {[this](const OrderUpdateEvent& e) { updates.push_back(e); },
                [this](const FillEvent& e) { fills.push_back(e); },
                [this](const ExchangeSnapshotEvent& e) { snapshots.push_back(e); },
                [this](const VenueCostEvent& e) { costs.push_back(e); }};
    }
};

std::string accountLog(const std::string& rows) { return R"({"accountUid":"x","logs":[)" + rows + "]}"; }

// One account history row; null fields as Kraken sends them.
std::string logRow(int id, const std::string& info, const std::string& fee, const std::string& funding,
                   const std::string& execution, const std::string& asset = "usd",
                   const std::string& date = "2026-10-12T00:00:05.000Z")
{
    return R"({"id":)" + std::to_string(id) + R"(,"date":")" + date + R"(","info":")" + info +
           R"(","asset":")" + asset + R"(","contract":"PF_XBTUSD","booking_uid":"b-)" + std::to_string(id) +
           R"(","execution":)" + (execution.empty() ? "null" : "\"" + execution + "\"") + R"(,"fee":)" + fee +
           R"(,"realized_funding":)" + funding + R"(,"margin_account":"flex","old_balance":1,"new_balance":1})";
}

// A history row that only moves a balance (transfer, conversion, interest).
std::string balanceRow(int id, const std::string& info, const std::string& asset, const std::string& oldBalance,
                       const std::string& newBalance, const std::string& wallet = "flex")
{
    return R"({"id":)" + std::to_string(id) + R"(,"date":"2026-10-12T00:00:05.000Z","info":")" + info +
           R"(","asset":")" + asset + R"(","contract":null,"execution":null,"fee":null,"realized_funding":null,)"
           R"("margin_account":")" + wallet + R"(","old_balance":)" + oldBalance + R"(,"new_balance":)" + newBalance + "}";
}

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
        transport.responses["account-log"] = {ok(accountLog(""))};
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
        // Spot-style cash: USD balance minus the entry value of the open positions
        // (0.002 x 60000 long, 7000 x 0.00001 short): 120.5 - (120 - 0.07).
        CHECK(near(event.snapshot.cash, 0.57) && event.snapshot.timestamp == TODAY);
        // BTC counts as collateral at Kraken's post-haircut value, never as cash.
        CHECK(near(event.snapshot.collateral_usd, 613.8));
        CHECK(near(event.snapshot.positions.at("BTCUSDT"), 0.002));
        CHECK(near(event.snapshot.positions.at("1000PEPEUSDT"), -7.0));
    }
    for (const HttpRequest& request : h.transport.requests)
        CHECK(request.method == "GET");
}

void testFeesAndFundingFromAccountLog()
{
    Harness h("costs");
    h.transport.responses["sendorder"] = {ok(PLACED)};
    h.transport.responses["fills"] = {ok(fills("")),
                                      ok(fills(fill("f-1", "algo-t1-7", "PF_XBTUSD", "buy", "0.0012", "62000.5")))};
    h.transport.responses["openorders"] = {ok(openOrders(""))};
    const std::string older = logRow(1, "futures trade", "0.5", "null", "f-old");
    const std::string fee = logRow(2, "futures trade", "0.0372", "null", "f-1");
    const std::string funding = logRow(3, "funding rate change", "null", "-0.0123", "");
    const std::string transfer = logRow(4, "transfer", "null", "null", "");
    const std::string btcFee = logRow(5, "futures trade", "0.000001", "null", "f-1", "xbt");
    const std::string early = logRow(6, "futures trade", "0.02", "null", "f-2", "usd", "2026-10-12T00:00:30.000Z");
    h.transport.responses["account-log"] = {
        ok(accountLog(older)),                                                          // baseline
        ok(accountLog(early + "," + transfer + "," + btcFee + "," + funding + "," + fee + "," + older))};

    h.submit(7, "BTCUSDT", OrderSide::Buy, 0.00123);
    h.poll();          // sent; the first history read is the baseline
    h.poll();          // fill f-1 recorded and published
    CHECK(h.events.fills.size() == 1 && h.events.costs.empty());
    h.poll(31000);     // history read again soon after the fill
    CHECK(h.events.costs.size() == 2);
    if (h.events.costs.size() == 2) {
        const VenueCostEvent& f = h.events.costs[0].kind == VenueCostKind::Fee ? h.events.costs[0] : h.events.costs[1];
        const VenueCostEvent& r = h.events.costs[0].kind == VenueCostKind::Fee ? h.events.costs[1] : h.events.costs[0];
        CHECK(f.cost_id == "kraken-futures:2:fee" && f.metadata.message_id == "kraken-futures-cost:2:fee");
        CHECK(near(f.amount_usd, -0.0372) && f.fill_id == 1 && f.coin == "BTCUSDT" && f.timestamp == TODAY);
        CHECK(r.kind == VenueCostKind::Funding && near(r.amount_usd, -0.0123) && r.fill_id == 0 && r.coin == "BTCUSDT");
    }

    const HttpRequest* read = nullptr;
    for (const HttpRequest& request : h.transport.requests)
        if (request.endpoint == "account-log")
            read = &request;
    CHECK(read != nullptr && read->method == "GET" &&
          read->url == "https://futures.kraken.com/api/history/v2/account-log?count=500");
    if (read != nullptr) {
        bool signedWithQuery = false;
        for (const auto& [name, value] : read->headers)
            signedWithQuery |= name == "Authent" &&
                value == KrakenFuturesProtocol::authent(SECRET, "count=500", "", "/api/history/v2/account-log");
        CHECK(signedWithQuery);
    }

    // The fee of f-2 (not yet polled) waits for its fill, then is booked on the account.
    h.poll(301000);
    CHECK(h.events.costs.size() == 2);
    h.poll(600000);
    CHECK(h.events.costs.size() == 3 && h.events.costs.back().fill_id == 0 && near(h.events.costs.back().amount_usd, -0.02));

    // Restart: nothing is published twice.
    const std::size_t before = h.events.costs.size();
    h.open(true);
    h.poll(301000);
    CHECK(h.events.costs.size() == before);
}

// User decision 2026-10-14: USD deposits, withdrawals and conversions are capital flows,
// venue interest a cost; BTC movements change only the collateral.
void testInterestAndCapitalFlowsFromAccountLog()
{
    Harness h("flows");
    const std::string deposit = balanceRow(10, "cross-exchange transfer", "usd", "100", "150");
    const std::string withdrawal = balanceRow(11, "transfer", "usd", "150", "140");
    const std::string conversion = balanceRow(12, "conversion", "usd", "140", "160.4");
    const std::string interest = balanceRow(13, "interest payment", "usd", "160.4", "160.37");
    const std::string btcIn = balanceRow(14, "cross-exchange transfer", "xbt", "0.01", "0.02");
    const std::string otherWallet = balanceRow(15, "transfer", "usd", "5", "0", "fi_xbtusd");
    const std::string unknown = balanceRow(16, "admin adjustment", "usd", "160.37", "161.37");
    const std::string noChange = balanceRow(17, "transfer", "usd", "161.37", "161.37");
    h.transport.responses["account-log"] = {
        ok(accountLog("")),                                                             // baseline
        ok(accountLog(deposit + "," + withdrawal + "," + conversion + "," + interest + "," + btcIn + "," +
                      otherWallet + "," + unknown + "," + noChange))};

    h.poll();          // baseline
    h.poll(301000);    // next scheduled history read
    std::map<std::string, VenueCostEvent> byId;
    for (const VenueCostEvent& cost : h.events.costs)
        byId[cost.cost_id] = cost;
    CHECK(h.events.costs.size() == 4);
    CHECK(byId.contains("kraken-futures:10:flow") && byId["kraken-futures:10:flow"].kind == VenueCostKind::CapitalFlow &&
          near(byId["kraken-futures:10:flow"].amount_usd, 50.0));
    CHECK(byId.contains("kraken-futures:11:flow") && near(byId["kraken-futures:11:flow"].amount_usd, -10.0));
    CHECK(byId.contains("kraken-futures:12:flow") && near(byId["kraken-futures:12:flow"].amount_usd, 20.4));
    CHECK(byId.contains("kraken-futures:13:interest") && byId["kraken-futures:13:interest"].kind == VenueCostKind::Interest &&
          near(byId["kraken-futures:13:interest"].amount_usd, -0.03) && byId["kraken-futures:13:interest"].coin.empty());

    // The parser keeps BTC and other-wallet entries apart from unknown USD changes.
    KrakenFuturesProtocol protocol("https://futures.kraken.com", CREDENTIALS);
    const AccountLog log = protocol.parseAccountLog(ok(accountLog(btcIn + "," + otherWallet + "," + unknown)));
    CHECK(log.entries.empty() && log.not_cash.size() == 2 && log.unaccounted == std::vector<std::string>{"admin adjustment"});

    // Restart: nothing is published twice.
    h.open(true);
    h.poll(301000);
    CHECK(h.events.costs.size() == 4);
}

// Shape of the real account history read on 2026-10-15 (read-only key): every row has all
// fields, null when unused; each trade has a USD row (fee, realized PnL and funding change
// the USD balance) and a row for the contract's position size; with no USD, Kraken converts
// BTC itself to pay a fee. Ids and uids are synthetic; amounts and words are the real ones.
std::string realLogRow(int id, const std::string& date, const std::string& info, const std::string& asset,
                       const std::string& contract, const std::string& oldBalance, const std::string& newBalance,
                       const std::string& fee, const std::string& realizedPnl, const std::string& funding,
                       bool execution)
{
    return R"({"id":)" + std::to_string(id) + R"(,"date":")" + date + R"(","asset":")" + asset +
           R"(","info":")" + info + R"(","booking_uid":"synthetic-b-)" + std::to_string(id) +
           R"(","margin_account":"flex","old_balance":)" + oldBalance + R"(,"new_balance":)" + newBalance +
           R"(,"old_average_entry_price":null,"new_average_entry_price":null,"trade_price":null,)"
           R"("mark_price":null,"realized_pnl":)" + realizedPnl + R"(,"fee":)" + fee +
           R"(,"execution":)" + (execution ? "\"synthetic-e-" + std::to_string(id) + "\"" : std::string("null")) +
           R"(,"collateral":null,"funding_rate":null,"realized_funding":)" + funding +
           R"(,"conversion_spread_percentage":null,"liquidation_fee":null,"position_uid":null,"contract":)" +
           (contract.empty() ? std::string("null") : "\"" + contract + "\"") + "}";
}

void testRealAccountLogShape()
{
    const std::string rows =
        // BTC moved in from the spot wallet: collateral only.
        realLogRow(1, "2026-10-07T10:55:19.012Z", "cross-exchange transfer", "xbt", "", "0.0", "0.22404114", "0.0", "null", "null", false) + "," +
        // Opening a short with no USD: Kraken sells a little BTC, the USD pays the fee.
        realLogRow(2, "2026-10-07T11:04:53.870Z", "conversion", "xbt", "", "0.224036644", "0.2240284352", "null", "null", "null", false) + "," +
        realLogRow(3, "2026-10-07T11:04:53.870Z", "conversion", "usd", "", "0.0", "0.6859", "null", "null", "null", false) + "," +
        realLogRow(4, "2026-10-07T11:04:53.870Z", "futures trade", "usd", "pf_xbtusd", "0.6859", "0.0", "0.6859", "0.0", "null", true) + "," +
        realLogRow(5, "2026-10-07T11:04:53.870Z", "futures trade", "pf_xbtusd", "pf_xbtusd", "0.0", "-0.0234", "null", "null", "null", true) + "," +
        // Reducing a short: realized PnL and the received funding are added to USD at once.
        realLogRow(6, "2026-10-07T11:23:04.857Z", "futures trade", "usd", "pf_hypeusd", "0.1631", "0.9818", "0.15", "0.9581", "0.0106", true) + "," +
        realLogRow(7, "2026-10-07T11:23:04.857Z", "futures trade", "pf_hypeusd", "pf_hypeusd", "-12.0", "-7.2", "null", "null", "null", true);

    // Kraken's USD balance: new = old + realized_pnl - fee + realized_funding.
    CHECK(near(0.1631 + 0.9581 - 0.15 + 0.0106, 0.9818) && near(0.6859 + 0.0 - 0.6859, 0.0));

    KrakenFuturesProtocol protocol("https://futures.kraken.com", CREDENTIALS);
    const AccountLog log = protocol.parseAccountLog(ok(accountLog(rows)));
    std::map<std::string, CashEntry> byId;
    for (const CashEntry& entry : log.entries)
        byId[entry.entry_id] = entry;
    CHECK(log.entries.size() == 4 && log.unaccounted.empty() && log.not_cash.size() == 2);
    CHECK(byId.contains("3:flow") && byId["3:flow"].kind == VenueCostKind::CapitalFlow && near(byId["3:flow"].amount_usd, 0.6859));
    CHECK(byId.contains("4:fee") && byId["4:fee"].kind == VenueCostKind::Fee && near(byId["4:fee"].amount_usd, -0.6859) &&
          byId["4:fee"].venue_fill_id == "synthetic-e-4" && byId["4:fee"].day == 20261007);
    CHECK(byId.contains("6:fee") && near(byId["6:fee"].amount_usd, -0.15));
    CHECK(byId.contains("6:funding") && byId["6:funding"].kind == VenueCostKind::Funding &&
          near(byId["6:funding"].amount_usd, 0.0106) && byId["6:funding"].symbol == "pf_hypeusd");
    // Booked USD changes plus the fill-derived realized PnL equal the balance changes.
    double booked = 0.0;
    for (const CashEntry& entry : log.entries)
        booked += entry.amount_usd;
    CHECK(near(booked + 0.9581, (0.6859 - 0.0) + (0.0 - 0.6859) + (0.9818 - 0.1631)));
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
    const auto history = protocol.parseAccountLog(transport.send(protocol.accountLogRequest()));

    const auto& snapshot = events.snapshots.at(0).snapshot;
    std::cout << "LIVE-READ-ONLY-PROBE: permissions read=" << permissions.read << " trade=" << permissions.trade
              << " withdraw=" << permissions.withdraw << "; snapshot parsed: " << snapshot.positions.size()
              << " positions, " << snapshot.open_orders.size() << " open orders; fills parsed: " << fills.size()
              << "; account history: " << history.entries.size() << " cash entries, "
              << history.not_cash.size() << " collateral-only, " << history.unaccounted.size()
              << " not booked; requests: GET only\n";
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
        {"fees and funding from the account history", testFeesAndFundingFromAccountLog},
        {"interest and capital flows from the account history", testInterestAndCapitalFlowsFromAccountLog},
        {"real account history shape (2026-10-15)", testRealAccountLogShape},
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
