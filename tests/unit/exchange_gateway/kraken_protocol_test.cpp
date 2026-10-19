// Kraken Futures protocol: signing, request bodies, answer parsing
// (tests/specs/exchange_gateway.md, Part K).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <gtest/gtest.h>

#include "test_support.h"
#include "venue_fakes.h"

using namespace venue_fakes;

namespace {

// Independent Base64 (RFC 4648) for the expected signature.
std::string decodeBase64(const std::string& text)
{
    static const std::string ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=')
            break;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(ALPHABET.find(c));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return output;
}

std::string encodeBase64(const unsigned char* data, std::size_t length)
{
    static const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < length; i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16) |
                                    (i + 1 < length ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                    (i + 2 < length ? static_cast<std::uint32_t>(data[i + 2]) : 0);
        output += ALPHABET[(chunk >> 18) & 63];
        output += ALPHABET[(chunk >> 12) & 63];
        output += i + 1 < length ? ALPHABET[(chunk >> 6) & 63] : '=';
        output += i + 2 < length ? ALPHABET[chunk & 63] : '=';
    }
    return output;
}

// Kraken's documented algorithm, computed here with OpenSSL one-shot functions.
std::string expectedAuthent(const std::string& postData, const std::string& nonce, const std::string& path)
{
    const std::string message = postData + nonce + path;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest);
    const std::string key = decodeBase64(SECRET);
    unsigned char mac[64];
    unsigned int macLength = 0;
    HMAC(EVP_sha512(), key.data(), static_cast<int>(key.size()), digest, sizeof(digest), mac, &macLength);
    return encodeBase64(mac, macLength);
}

KrakenFuturesProtocol protocol() { return KrakenFuturesProtocol(HOST, CREDENTIALS); }

OrderRequest marketOrder(double quantity = 0.0123)
{
    OrderRequest order;
    order.local_order_id = 7;
    order.client_order_id = "algo-t1-7";
    order.symbol = "PF_XBTUSD";
    order.side = OrderSide::Buy;
    order.quantity = quantity;
    return order;
}

} // namespace


// ---------------------------------------------------------------- K1 signing

TEST(KrakenProtocol, K1_AuthentMatchesPythonReferenceValues)
{
    // Computed with Python hashlib/hmac/base64 for the synthetic secret (bytes 0..63).
    EXPECT_EQ(KrakenFuturesProtocol::authent(
                  SECRET, "orderType=mkt&symbol=PF_XBTUSD&side=buy&size=0.0123&cliOrdId=algo-t1-7", "1791763205000",
                  "/api/v3/sendorder"),
              "xWdzTYhDYi35xzsfd3URxie0DACsJZuUin3cpVw831cULartAGUkV+frR32cjFU4QwnnTI6ETbzlVPEQE2KrEQ==");
    EXPECT_EQ(KrakenFuturesProtocol::authent(SECRET, "", "", "/api/v3/openorders"),
              "01NpmRljuNFOYCFDW+OXrtHSByZQJbicy27eoNrFl8w05mo30EzFYxJjL+yj7ul6RPZy5zHB0z3sIuPz2gycng==");
    EXPECT_EQ(KrakenFuturesProtocol::authent(SECRET, "count=500", "", "/api/history/v2/account-log"),
              "I+iJNEhu5oEuHgzG4jFTAkUs7POzogCIjn1wd0qPkccnNOgSAyMn09kO2Nun+sIpiVP1m6goFzFL+5Ysw+5BLQ==");
}

TEST(KrakenProtocol, K1_AuthentMatchesIndependentOpenSslComputation)
{
    const std::string body = "cliOrdId=algo-t1-42";
    EXPECT_EQ(KrakenFuturesProtocol::authent(SECRET, body, "1791763209999", "/api/v3/cancelorder"),
              expectedAuthent(body, "1791763209999", "/api/v3/cancelorder"));
}

TEST(KrakenProtocol, K1_InvalidSecretIsRefused)
{
    EXPECT_THROW(KrakenFuturesProtocol::authent("not base64!", "", "", "/api/v3/fills"), std::invalid_argument);
    EXPECT_THROW(KrakenFuturesProtocol(HOST, {"testkey12345", "c2hvcnQ="}), std::invalid_argument); // 5 bytes
}

// ---------------------------------------------------------------- K2 / K3 / K4 requests

TEST(KrakenProtocol, K2_SendOrderIsASignedPostWithIncreasingNonces)
{
    KrakenFuturesProtocol kraken = protocol();
    const HttpRequest first = kraken.submitRequest(marketOrder());
    const HttpRequest second = kraken.cancelRequest("algo-t1-7");

    EXPECT_EQ(first.method, "POST");
    EXPECT_EQ(first.url, "https://futures.kraken.com/derivatives/api/v3/sendorder");
    EXPECT_EQ(header(first, "APIKey"), "testkey12345");
    const std::string nonce = header(first, "Nonce");
    ASSERT_FALSE(nonce.empty());
    EXPECT_EQ(header(first, "Authent"), expectedAuthent(first.body, nonce, "/api/v3/sendorder"));

    EXPECT_EQ(second.method, "POST");
    EXPECT_EQ(second.url, "https://futures.kraken.com/derivatives/api/v3/cancelorder");
    EXPECT_EQ(second.body, "cliOrdId=algo-t1-7");
    EXPECT_GT(std::stoll(header(second, "Nonce")), std::stoll(nonce));
    EXPECT_EQ(header(second, "Authent"), expectedAuthent(second.body, header(second, "Nonce"), "/api/v3/cancelorder"));
}

TEST(KrakenProtocol, K3_PrivateGetsHaveNoNonceAndSignThePathOnly)
{
    KrakenFuturesProtocol kraken = protocol();
    const std::vector<std::pair<HttpRequest, std::string>> gets = {
        {kraken.openOrdersRequest(), "openorders"},
        {kraken.fillsRequest(), "fills"},
        {kraken.accountsRequest(), "accounts"},
        {kraken.positionsRequest(), "openpositions"}};
    for (const auto& [request, endpoint] : gets) {
        EXPECT_EQ(request.method, "GET") << endpoint;
        EXPECT_EQ(request.url, "https://futures.kraken.com/derivatives/api/v3/" + endpoint);
        EXPECT_FALSE(hasHeader(request, "Nonce")) << endpoint;
        EXPECT_EQ(header(request, "APIKey"), "testkey12345");
        EXPECT_EQ(header(request, "Authent"), expectedAuthent("", "", "/api/v3/" + endpoint)) << endpoint;
    }
    EXPECT_EQ(kraken.instrumentsRequest().method, "GET");
    EXPECT_EQ(kraken.tickersRequest().method, "GET");
    EXPECT_EQ(kraken.permissionsRequest().method, "GET");
}

TEST(KrakenProtocol, K4_AccountLogSignsItsQueryWithoutNonce)
{
    KrakenFuturesProtocol kraken = protocol();
    const HttpRequest request = kraken.accountLogRequest();
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.url, "https://futures.kraken.com/api/history/v2/account-log?count=500");
    EXPECT_FALSE(hasHeader(request, "Nonce"));
    EXPECT_EQ(header(request, "Authent"), expectedAuthent("count=500", "", "/api/history/v2/account-log"));
}

// ---------------------------------------------------------------- K5 / K6 bodies

TEST(KrakenProtocol, K5_MarketOrderBody)
{
    KrakenFuturesProtocol kraken = protocol();
    const HttpRequest request = kraken.submitRequest(marketOrder());
    EXPECT_EQ(request.body, "orderType=mkt&symbol=PF_XBTUSD&side=buy&size=0.0123&cliOrdId=algo-t1-7");

    OrderRequest sell = marketOrder(5000.0);
    sell.side = OrderSide::Sell;
    sell.symbol = "PF_PEPEUSD";
    EXPECT_EQ(kraken.submitRequest(sell).body, "orderType=mkt&symbol=PF_PEPEUSD&side=sell&size=5000&cliOrdId=algo-t1-7");
}

TEST(KrakenProtocol, K5_InvalidOrdersAreRefused)
{
    KrakenFuturesProtocol kraken = protocol();
    EXPECT_THROW(kraken.submitRequest(marketOrder(0.0)), std::invalid_argument);
    EXPECT_THROW(kraken.submitRequest(marketOrder(-1.0)), std::invalid_argument);
    OrderRequest longId = marketOrder();
    longId.client_order_id = std::string(101, 'a');
    EXPECT_THROW(kraken.submitRequest(longId), std::invalid_argument);
}

TEST(KrakenProtocol, K6_PositionStopIsReduceOnlyMarkTriggeredStopMarket)
{
    KrakenFuturesProtocol kraken = protocol();
    OrderRequest stop = marketOrder(0.1);
    stop.side = OrderSide::Sell;
    stop.type = VenueOrderType::PositionStop;
    stop.stop_price = 43400.0;
    const HttpRequest request = kraken.submitRequest(stop);
    EXPECT_EQ(formValue(request.body, "orderType"), "stp");
    EXPECT_EQ(formValue(request.body, "symbol"), "PF_XBTUSD");
    EXPECT_EQ(formValue(request.body, "side"), "sell");
    EXPECT_EQ(formValue(request.body, "size"), "0.1");
    EXPECT_EQ(formValue(request.body, "cliOrdId"), "algo-t1-7");
    EXPECT_DOUBLE_EQ(std::stod(formValue(request.body, "stopPrice")), 43400.0);
    EXPECT_EQ(formValue(request.body, "reduceOnly"), "true");
    EXPECT_EQ(formValue(request.body, "triggerSignal"), "mark");
    EXPECT_FALSE(formHas(request.body, "limitPrice"));
}

TEST(KrakenProtocol, K6_BuyOrPricelessStopIsRefused)
{
    KrakenFuturesProtocol kraken = protocol();
    OrderRequest stop = marketOrder(0.1);
    stop.type = VenueOrderType::PositionStop;
    stop.stop_price = 43400.0;
    stop.side = OrderSide::Buy;
    EXPECT_THROW(kraken.submitRequest(stop), std::invalid_argument);
    stop.side = OrderSide::Sell;
    stop.stop_price = 0.0;
    EXPECT_THROW(kraken.submitRequest(stop), std::invalid_argument);
}

// ---------------------------------------------------------------- K7 / K8 outcomes

TEST(KrakenProtocol, K7_AcceptedStatusesCarryTheVenueOrderId)
{
    const KrakenFuturesProtocol kraken = protocol();
    for (const char* status : {"placed", "partiallyFilled", "filled"}) {
        const SubmitResult result = kraken.parseSubmit(
            ok(std::string(R"({"result":"success","sendStatus":{"order_id":"abc-1","status":")") + status + "\"}}"));
        EXPECT_EQ(result.outcome, SubmitOutcome::Accepted) << status;
        EXPECT_EQ(result.venue_order_id, "abc-1");
    }
}

TEST(KrakenProtocol, K7_NotProcessedAnswersAreSafeToResend)
{
    const KrakenFuturesProtocol kraken = protocol();
    EXPECT_EQ(kraken.parseSubmit(httpStatus(429)).outcome, SubmitOutcome::NotProcessed);
    for (const char* code : {"apiLimitExceeded", "nonceBelowThreshold", "nonceDuplicate"})
        EXPECT_EQ(kraken.parseSubmit(ok(venueError(code))).outcome, SubmitOutcome::NotProcessed) << code;
}

TEST(KrakenProtocol, K7_AmbiguousAnswersAreUnknown)
{
    const KrakenFuturesProtocol kraken = protocol();
    EXPECT_EQ(kraken.parseSubmit(noAnswer()).outcome, SubmitOutcome::Unknown);
    EXPECT_EQ(kraken.parseSubmit(httpStatus(500)).outcome, SubmitOutcome::Unknown);
    EXPECT_EQ(kraken.parseSubmit(httpStatus(503)).outcome, SubmitOutcome::Unknown);
    for (const char* code : {"Unavailable", "Server Error", "unknownError"})
        EXPECT_EQ(kraken.parseSubmit(ok(venueError(code))).outcome, SubmitOutcome::Unknown) << code;
    EXPECT_EQ(kraken.parseSubmit(ok(R"({"result":"error"})")).outcome, SubmitOutcome::Unknown);
    EXPECT_EQ(kraken.parseSubmit(ok(sendStatus("clientOrderIdAlreadyExist"))).outcome, SubmitOutcome::Unknown);
}

TEST(KrakenProtocol, K7_DefinitiveRefusalsAreRejected)
{
    const KrakenFuturesProtocol kraken = protocol();
    EXPECT_EQ(kraken.parseSubmit(httpStatus(400)).outcome, SubmitOutcome::Rejected);
    EXPECT_EQ(kraken.parseSubmit(httpStatus(401)).outcome, SubmitOutcome::Rejected);
    EXPECT_EQ(kraken.parseSubmit(ok(venueError("invalidArgument"))).outcome, SubmitOutcome::Rejected);
    const SubmitResult funds = kraken.parseSubmit(ok(sendStatus("insufficientAvailableFunds")));
    EXPECT_EQ(funds.outcome, SubmitOutcome::Rejected);
    EXPECT_EQ(funds.reason, "insufficientAvailableFunds");
}

TEST(KrakenProtocol, K8_CancelAnswers)
{
    const KrakenFuturesProtocol kraken = protocol();
    const auto cancel = [&](const std::string& status) {
        return kraken.parseCancel(ok(R"({"result":"success","cancelStatus":{"status":")" + status + "\"}}")).outcome;
    };
    EXPECT_EQ(cancel("cancelled"), CancelOutcome::Cancelled);
    EXPECT_EQ(cancel("filled"), CancelOutcome::AlreadyFilled);
    EXPECT_EQ(cancel("notFound"), CancelOutcome::NotFound);
    EXPECT_EQ(cancel("somethingElse"), CancelOutcome::Unknown);
    EXPECT_EQ(kraken.parseCancel(noAnswer()).outcome, CancelOutcome::Unknown);
    EXPECT_EQ(kraken.parseCancel(httpStatus(502)).outcome, CancelOutcome::Unknown);
    EXPECT_EQ(kraken.parseCancel(ok(venueError("apiLimitExceeded"))).outcome, CancelOutcome::Unknown);
}

// ---------------------------------------------------------------- K9 / K10 markets and prices

TEST(KrakenProtocol, K9_OnlyMultiMPerpetualsWithStepsAndReasons)
{
    const std::string body = R"({"result":"success","instruments":[
     {"symbol":"PF_XBTUSD","type":"flexible_futures","base":"XBT","quote":"USD","tradeable":true,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":4,"tickSize":1},
     {"symbol":"PF_PEPEUSD","type":"flexible_futures","base":"PEPE","quote":"USD","tradeable":true,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.0000000001},
     {"symbol":"PF_OLDUSD","type":"flexible_futures","base":"OLD","quote":"USD","tradeable":true,"isExpired":true,
      "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01},
     {"symbol":"PF_HALTUSD","type":"flexible_futures","base":"HALT","quote":"USD","tradeable":false,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01},
     {"symbol":"PF_BIGUSD","type":"flexible_futures","base":"BIG","quote":"USD","tradeable":true,"isExpired":false,
      "contractSize":10,"contractValueTradePrecision":0,"tickSize":0.01},
     {"symbol":"PF_NOTICKUSD","type":"flexible_futures","base":"NOTICK","quote":"USD","tradeable":true,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":0},
     {"symbol":"PF_AAPLXUSD","type":"flexible_futures","base":"AAPLX","quote":"USD","tradeable":true,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":2,"tickSize":0.01,"tradfi":true},
     {"symbol":"FI_XBTUSD_261225","type":"futures_inverse","base":"XBT","quote":"USD","tradeable":true,
      "isExpired":false,"contractSize":1,"contractValueTradePrecision":0,"tickSize":0.5},
     {"symbol":"PF_EURUSD","type":"flexible_futures","base":"EUR","quote":"EUR","tradeable":true,"isExpired":false,
      "contractSize":1,"contractValueTradePrecision":0,"tickSize":0.01}]})";
    const std::vector<Market> markets = protocol().parseMarkets(ok(body));
    std::map<std::string, Market> bySymbol;
    for (const Market& market : markets)
        bySymbol[market.symbol] = market;

    EXPECT_FALSE(bySymbol.contains("PF_AAPLXUSD"));
    EXPECT_FALSE(bySymbol.contains("FI_XBTUSD_261225"));
    EXPECT_FALSE(bySymbol.contains("PF_EURUSD"));

    ASSERT_TRUE(bySymbol.contains("PF_XBTUSD"));
    EXPECT_EQ(bySymbol["PF_XBTUSD"].asset, "BTC");
    EXPECT_TRUE(bySymbol["PF_XBTUSD"].trading);
    EXPECT_NEAR(bySymbol["PF_XBTUSD"].quantity_step, 0.0001, 1e-15);
    EXPECT_DOUBLE_EQ(bySymbol["PF_XBTUSD"].tick_size, 1.0);
    EXPECT_TRUE(bySymbol["PF_PEPEUSD"].trading);
    EXPECT_DOUBLE_EQ(bySymbol["PF_PEPEUSD"].quantity_step, 1.0);

    for (const char* symbol : {"PF_OLDUSD", "PF_HALTUSD", "PF_BIGUSD", "PF_NOTICKUSD"}) {
        ASSERT_TRUE(bySymbol.contains(symbol)) << symbol;
        EXPECT_FALSE(bySymbol[symbol].trading) << symbol;
        EXPECT_FALSE(bySymbol[symbol].unavailable_reason.empty()) << symbol;
    }
}

TEST(KrakenProtocol, K9_NoPerpetualsIsAnError)
{
    EXPECT_THROW(protocol().parseMarkets(ok(R"({"result":"success","instruments":[]})")), ProtocolError);
    EXPECT_THROW(protocol().parseMarkets(ok(venueError("Unavailable"))), ProtocolError);
    EXPECT_THROW(protocol().parseMarkets(httpStatus(500)), ProtocolError);
}

TEST(KrakenProtocol, K10_MarkPricesSkipSuspendedAndNonPerpetuals)
{
    const auto prices = protocol().parseMarkPrices(ok(R"({"result":"success","tickers":[
     {"symbol":"PF_XBTUSD","markPrice":62000.0,"suspended":false},
     {"symbol":"PF_HALTUSD","markPrice":3.0,"suspended":true},
     {"symbol":"FI_XBTUSD_261225","markPrice":62500.0},
     {"symbol":"PF_ETHUSD","markPrice":"2500.5"},
     {"symbol":"PF_NOMARKUSD"}]})"));
    EXPECT_EQ(prices.size(), 2u);
    EXPECT_DOUBLE_EQ(prices.at("PF_XBTUSD"), 62000.0);
    EXPECT_DOUBLE_EQ(prices.at("PF_ETHUSD"), 2500.5);
}

// ---------------------------------------------------------------- K11 fills and open orders

TEST(KrakenProtocol, K11_FillsAreParsedWithUtcTimeAndDay)
{
    const auto fills = protocol().parseFills(ok(fillsBody(
        {fillRow("F1", "algo-t1-7", "PF_XBTUSD", "buy", "0.04", "62010.5"),
         fillRow("F2", "", "PF_ETHUSD", "sell", "1.5", "2499", "2026-10-11T23:59:59Z", "venue-order-9")})));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].venue_fill_id, "F1");
    EXPECT_EQ(fills[0].venue_order_id, "venue-order-1");
    EXPECT_EQ(fills[0].client_order_id, "algo-t1-7");
    EXPECT_EQ(fills[0].symbol, "PF_XBTUSD");
    EXPECT_EQ(fills[0].side, OrderSide::Buy);
    EXPECT_DOUBLE_EQ(fills[0].quantity, 0.04);
    EXPECT_DOUBLE_EQ(fills[0].price, 62010.5);
    EXPECT_EQ(fills[0].day, 20261012u);
    EXPECT_EQ(fills[0].fill_time_ms, 1791763207250LL);
    EXPECT_EQ(fills[0].fill_type, "taker");

    EXPECT_TRUE(fills[1].client_order_id.empty());
    EXPECT_EQ(fills[1].side, OrderSide::Sell);
    EXPECT_EQ(fills[1].day, 20261011u);
    EXPECT_EQ(fills[1].fill_time_ms, 1791763199000LL);
}

TEST(KrakenProtocol, K11_InvalidFillsAreProtocolErrors)
{
    const KrakenFuturesProtocol kraken = protocol();
    EXPECT_THROW(kraken.parseFills(ok(fillsBody({fillRow("F1", "", "PF_XBTUSD", "buy", "0", "62000")}))),
                 ProtocolError);
    EXPECT_THROW(kraken.parseFills(ok(fillsBody({fillRow("F1", "", "PF_XBTUSD", "buy", "0.1", "-1")}))),
                 ProtocolError);
    EXPECT_THROW(kraken.parseFills(ok(fillsBody({fillRow("F1", "", "PF_XBTUSD", "buy", "0.1", "62000",
                                                         "2026-10-12T02:00:00+02:00")}))),
                 ProtocolError);
    EXPECT_THROW(kraken.parseFills(ok(fillsBody({fillRow("F1", "", "PF_XBTUSD", "hold", "0.1", "62000")}))),
                 ProtocolError);
    EXPECT_THROW(kraken.parseFills(ok(venueError("authenticationError"))), ProtocolError);
}

TEST(KrakenProtocol, K11_OpenOrdersAreParsed)
{
    const auto orders = protocol().parseOpenOrders(ok(openOrdersBody(
        {openOrderRow("algo-t1-7", "PF_XBTUSD", "sell", "0.02", "0.08", "v-7"),
         R"({"order_id":"v-8","symbol":"PF_ETHUSD","side":"buy","filledSize":0,"unfilledSize":1})"})));
    ASSERT_EQ(orders.size(), 2u);
    EXPECT_EQ(orders[0].venue_order_id, "v-7");
    EXPECT_EQ(orders[0].client_order_id, "algo-t1-7");
    EXPECT_EQ(orders[0].side, OrderSide::Sell);
    EXPECT_DOUBLE_EQ(orders[0].filled, 0.02);
    EXPECT_DOUBLE_EQ(orders[0].unfilled, 0.08);
    EXPECT_TRUE(orders[1].client_order_id.empty());
}

// ---------------------------------------------------------------- K12 account

TEST(KrakenProtocol, K12_AccountBalanceCollateralAndSignedPositions)
{
    const AccountState state = protocol().parseAccount(
        ok(R"({"result":"success","accounts":{"flex":{"type":"multiCollateralMarginAccount","currencies":{
            "USD":{"quantity":10000.0,"value":10000.0,"collateral":10000.0},
            "XBT":{"quantity":0.01,"value":620.0,"collateral":613.8}}}}})"),
        ok(R"({"result":"success","openPositions":[
            {"symbol":"PF_XBTUSD","side":"long","size":0.1,"price":60000.0},
            {"symbol":"PF_PEPEUSD","side":"short","size":2000,"price":0.00001}]})"));
    EXPECT_DOUBLE_EQ(state.usd_balance, 10000.0);
    EXPECT_DOUBLE_EQ(state.collateral_usd, 613.8);
    ASSERT_EQ(state.positions.size(), 2u);
    EXPECT_EQ(state.positions[0].symbol, "PF_XBTUSD");
    EXPECT_DOUBLE_EQ(state.positions[0].signed_quantity, 0.1);
    EXPECT_DOUBLE_EQ(state.positions[0].entry_price, 60000.0);
    EXPECT_DOUBLE_EQ(state.positions[1].signed_quantity, -2000.0);
}

TEST(KrakenProtocol, K12_OtherWalletTypeIsRefused)
{
    EXPECT_THROW(protocol().parseAccount(
                     ok(R"({"result":"success","accounts":{"flex":{"type":"cashAccount","currencies":{}}}})"),
                     ok(R"({"result":"success","openPositions":[]})")),
                 ProtocolError);
}

// ---------------------------------------------------------------- K13 account history

TEST(KrakenProtocol, K13_AccountHistoryBecomesCashEntries)
{
    const std::string body = R"({"logs":[
     {"id":101,"date":"2026-10-12T00:00:08.000Z","asset":"usd","info":"futures trade","fee":0.75,"execution":"F1",
      "realized_funding":null,"contract":"pf_xbtusd","old_balance":1000.0,"new_balance":999.25,"margin_account":"flex"},
     {"id":102,"date":"2026-10-12T08:00:00.000Z","asset":"usd","info":"futures trade","fee":null,"execution":null,
      "realized_funding":0.0106,"contract":"pf_strkusd","old_balance":999.25,"new_balance":999.2606,"margin_account":"flex"},
     {"id":103,"date":"2026-10-12T09:00:00.000Z","asset":"usd","info":"interest payment","fee":null,"execution":null,
      "realized_funding":null,"contract":null,"old_balance":1000.0,"new_balance":999.5,"margin_account":"flex"},
     {"id":104,"date":"2026-10-12T10:00:00.000Z","asset":"usd","info":"cross-exchange transfer","fee":null,
      "execution":null,"realized_funding":null,"contract":null,"old_balance":0.0,"new_balance":500.0,"margin_account":"flex"},
     {"id":105,"date":"2026-10-12T11:00:00.000Z","asset":"xbt","info":"cross-exchange transfer","fee":null,
      "execution":null,"realized_funding":null,"contract":null,"old_balance":0.0,"new_balance":0.01,"margin_account":"flex"},
     {"id":106,"date":"2026-10-12T12:00:00.000Z","asset":"usd","info":"futures trade","fee":null,"execution":null,
      "realized_funding":null,"contract":"pf_xbtusd","old_balance":500.0,"new_balance":510.0,"margin_account":"flex"},
     {"id":107,"date":"2026-10-12T13:00:00.000Z","asset":"usd","info":"mystery adjustment","fee":null,"execution":null,
      "realized_funding":null,"contract":null,"old_balance":510.0,"new_balance":509.0,"margin_account":"flex"}]})";
    const AccountLog log = protocol().parseAccountLog(ok(body));

    std::map<std::string, CashEntry> byId;
    for (const CashEntry& entry : log.entries)
        byId[entry.entry_id] = entry;
    ASSERT_EQ(byId.size(), 4u);

    ASSERT_TRUE(byId.contains("101:fee"));
    EXPECT_EQ(byId["101:fee"].kind, VenueCostKind::Fee);
    EXPECT_DOUBLE_EQ(byId["101:fee"].amount_usd, -0.75);
    EXPECT_EQ(byId["101:fee"].venue_fill_id, "F1");
    EXPECT_EQ(byId["101:fee"].day, 20261012u);

    ASSERT_TRUE(byId.contains("102:funding"));
    EXPECT_EQ(byId["102:funding"].kind, VenueCostKind::Funding);
    EXPECT_DOUBLE_EQ(byId["102:funding"].amount_usd, 0.0106);
    EXPECT_EQ(byId["102:funding"].symbol, "PF_STRKUSD");

    ASSERT_TRUE(byId.contains("103:interest"));
    EXPECT_EQ(byId["103:interest"].kind, VenueCostKind::Interest);
    EXPECT_NEAR(byId["103:interest"].amount_usd, -0.5, 1e-12);

    ASSERT_TRUE(byId.contains("104:flow"));
    EXPECT_EQ(byId["104:flow"].kind, VenueCostKind::CapitalFlow);
    EXPECT_DOUBLE_EQ(byId["104:flow"].amount_usd, 500.0);

    EXPECT_EQ(log.not_cash.size(), 1u);
    EXPECT_EQ(log.unaccounted.size(), 1u);
}

// ---------------------------------------------------------------- K14 permissions and ids

TEST(KrakenProtocol, K14_PermissionsFlagTradeAndFundMovement)
{
    const KrakenFuturesProtocol kraken = protocol();
    const Permissions full = kraken.parsePermissions(ok(R"({"permissions":{"general":"FULL_ACCESS","transfer":"NO_ACCESS"}})"));
    EXPECT_TRUE(full.read);
    EXPECT_TRUE(full.trade);
    EXPECT_FALSE(full.withdraw);

    const Permissions readOnly = kraken.parsePermissions(ok(R"({"permissions":{"general":"READ_ONLY"}})"));
    EXPECT_TRUE(readOnly.read);
    EXPECT_FALSE(readOnly.trade);
    EXPECT_FALSE(readOnly.withdraw);

    const Permissions withdraw =
        kraken.parsePermissions(ok(R"({"permissions":{"general":"READ_ONLY","withdrawal":"FULL_ACCESS"}})"));
    EXPECT_TRUE(withdraw.withdraw);
}

TEST(KrakenProtocol, K14_ClientOrderIdsAreNamespaced)
{
    const KrakenFuturesProtocol kraken = protocol();
    EXPECT_EQ(kraken.clientOrderId("k20261011", 42), "algo-k20261011-42");
    EXPECT_TRUE(kraken.isOurClientOrderId("k20261011", "algo-k20261011-42"));
    EXPECT_FALSE(kraken.isOurClientOrderId("k20261011", "algo-k20261012-42"));
    EXPECT_FALSE(kraken.isOurClientOrderId("k1", "algo-k10-42"));
    EXPECT_FALSE(kraken.isOurClientOrderId("k20261011", "manual-order"));
    for (const char* bad : {"", "K1", "a-b", "abcdefghijabcdefghijabcde"})
        EXPECT_THROW(kraken.clientOrderId(bad, 1), std::invalid_argument) << "'" << bad << "'";
}

TEST(KrakenProtocol, K14_HostAndKeyFormatsAreChecked)
{
    EXPECT_THROW(KrakenFuturesProtocol("http://futures.kraken.com", CREDENTIALS), std::invalid_argument);
    EXPECT_THROW(KrakenFuturesProtocol(HOST, {"short", SECRET}), std::invalid_argument);
    EXPECT_THROW(KrakenFuturesProtocol(HOST, {"testkey12345", "not-base64"}), std::invalid_argument);
    EXPECT_NO_THROW(KrakenFuturesProtocol(HOST, CREDENTIALS));
}

TEST(KrakenProtocol, K14_SyntheticCredentialFileNeedsExactlyTheTwoVariables)
{
    test_support::TempDir dir("kraken-credentials");
    const auto path = dir / "kraken.env";
    test_support::writeFile(path, "KRAKEN_API_KEY=testkey12345\nKRAKEN_API_SECRET=" + SECRET + "\n");
    const KrakenCredentials loaded = loadKrakenCredentials(path.string());
    EXPECT_EQ(loaded.api_key, "testkey12345");
    EXPECT_EQ(loaded.api_secret, SECRET);

    test_support::writeFile(path, "KRAKEN_API_KEY=testkey12345\n");
    EXPECT_THROW(loadKrakenCredentials(path.string()), std::runtime_error);
    test_support::writeFile(path, "KRAKEN_API_KEY=a\nKRAKEN_API_SECRET=b\nOTHER=c\n");
    EXPECT_THROW(loadKrakenCredentials(path.string()), std::runtime_error);
    test_support::writeFile(path, "KRAKEN_API_KEY=a\nKRAKEN_API_KEY=b\nKRAKEN_API_SECRET=c\n");
    EXPECT_THROW(loadKrakenCredentials(path.string()), std::runtime_error);
}
