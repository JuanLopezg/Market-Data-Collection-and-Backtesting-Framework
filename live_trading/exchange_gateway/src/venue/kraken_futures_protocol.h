#pragma once

#include <string>

#include "venue_protocol.h"

// Kraken Futures (Multi-M / flexible perpetuals, "PF_" symbols) REST protocol.
//
// Sources (checked 2026-10-11): docs.kraken.com Futures REST reference (sendorder,
// cancelorder, openorders, fills, orders/status, batchorder, instruments), the
// Futures authentication guide and the Futures rate-limit guide. Details that the
// documentation leaves open are marked "UNCONFIRMED" and must be verified with the
// first authorized small real-money test before they are relied upon.
//
// What is Kraken-specific here:
// - Authent = Base64(HMAC-SHA512(Base64Decode(secret), SHA256(postData + nonce + path))),
//   where path omits the "/derivatives" prefix (as accepted by the read-only account tool);
// - XBT is Kraken's code for BTC; contracts are "PF_<asset>USD" with contractSize 1;
// - sizes use contractValueTradePrecision decimals; market orders are "mkt"
//   (immediate-or-cancel with Kraken's 1% price protection);
// - client order ids ("cliOrdId", at most 100 characters) are "algo-<namespace>-<order id>";
// - budget: 500 cost units per 10 seconds (sendorder/cancelorder 10, reads 2).
namespace Venue {

struct KrakenCredentials {
    std::string api_key;
    std::string api_secret; // Base64, as issued by Kraken.
};

// Reads exactly KRAKEN_API_KEY and KRAKEN_API_SECRET from a dedicated file.
// Error messages are constant and never contain file contents.
KrakenCredentials loadKrakenCredentials(const std::string& path);

class KrakenFuturesProtocol final : public VenueProtocol {
public:
    // host: "https://futures.kraken.com" (production; there is no supported demo).
    KrakenFuturesProtocol(std::string host, KrakenCredentials credentials);

    std::string name() const override { return "kraken-futures"; }
    RateBudget rateBudget() const override { return {500, 10000}; }
    int cost(Endpoint endpoint) const override;

    std::string clientOrderId(const std::string& accountNamespace, OrderID localOrderId) const override;
    bool isOurClientOrderId(const std::string& accountNamespace, const std::string& clientOrderId) const override;

    HttpRequest permissionsRequest() override;
    Permissions parsePermissions(const HttpResponse& response) const override;

    HttpRequest instrumentsRequest() override;
    std::vector<Market> parseMarkets(const HttpResponse& response) const override;

    HttpRequest submitRequest(const OrderRequest& order) override;
    SubmitResult parseSubmit(const HttpResponse& response) const override;

    HttpRequest cancelRequest(const std::string& clientOrderId) override;
    CancelResult parseCancel(const HttpResponse& response) const override;

    HttpRequest openOrdersRequest() override;
    std::vector<OpenOrder> parseOpenOrders(const HttpResponse& response) const override;

    HttpRequest fillsRequest() override;
    std::vector<VenueFill> parseFills(const HttpResponse& response) const override;

    HttpRequest accountsRequest() override;
    HttpRequest positionsRequest() override;
    AccountState parseAccount(const HttpResponse& accounts, const HttpResponse& positions) const override;

    std::optional<HttpRequest> deadMansSwitchRequest(int timeoutSeconds) override;

    // Exposed for tests: the documented signing algorithm.
    static std::string authent(const std::string& secretBase64, const std::string& postData,
                               const std::string& nonce, const std::string& signingPath);

private:
    std::string host_;
    KrakenCredentials credentials_;
    long long last_nonce_ = 0;

    HttpRequest privateGet(const std::string& endpoint, const std::string& name);
    HttpRequest privatePost(const std::string& endpoint, const std::string& name, const std::string& body);
    std::string nextNonce();
};

} // namespace Venue
