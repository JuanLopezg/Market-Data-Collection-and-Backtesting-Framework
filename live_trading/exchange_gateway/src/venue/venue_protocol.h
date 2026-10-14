#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "data_types.h"
#include "execution_order.h"
#include "http_transport.h"

// Real-venue integration: what every exchange shares and what each one replaces.
//
//   LiveVenueAdapter (common)      order lifecycle, durable journal, submission gate,
//                                  ambiguous-outcome handling, fill de-duplication,
//                                  rate budget, snapshots, events to the gateway.
//   VenueProtocol (per exchange)   authentication, endpoints, symbols and units, request
//                                  formats, response parsing, error/status mapping, costs.
//
// Adding an exchange means writing one VenueProtocol. The adapter never branches on the
// venue name; it only uses the normalized types below.
namespace Venue {

// Source symbols (BTCUSDT, 1000PEPEUSDT) become venue symbols and units through the explicit
// SymbolMap (symbol_map.h); protocols only speak venue symbols.

// ------------------------------------------------------------- normalized data
enum class Endpoint {
    Permissions,
    Instruments,
    Tickers,
    Accounts,
    Positions,
    OpenOrders,
    Fills,
    SendOrder,
    CancelOrder,
    DeadMansSwitch
};

const char* endpointName(Endpoint endpoint);

struct Market {
    std::string asset;        // Underlying asset, e.g. "BTC" (Kraken's XBT).
    std::string symbol;       // Venue symbol, e.g. "PF_XBTUSD".
    bool trading = false;     // Listed, not expired and not suspended.
    double quantity_step = 0; // Smallest order size increment in underlying units.
    double tick_size = 0;
    std::string unavailable_reason; // Set when trading is false.
};

struct Permissions {
    bool read = false;
    bool trade = false;
    bool withdraw = false;
};

// Market order in venue units, already rounded to the venue quantity step.
struct OrderRequest {
    OrderID local_order_id = 0;
    std::string client_order_id;
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;
};

enum class SubmitOutcome {
    Accepted,     // The venue placed the order.
    Rejected,     // The venue refused it for good; nothing is open.
    NotProcessed, // Refused before processing (rate limit, nonce); safe to send again later.
    Unknown       // No reliable answer (timeout, 5xx, malformed). Resolve with evidence; never resend blindly.
};

struct SubmitResult {
    SubmitOutcome outcome = SubmitOutcome::Unknown;
    std::string venue_order_id;
    std::string reason;
};

enum class CancelOutcome { Cancelled, AlreadyFilled, NotFound, Unknown };

struct CancelResult {
    CancelOutcome outcome = CancelOutcome::Unknown;
    std::string reason;
};

struct OpenOrder {
    std::string venue_order_id;
    std::string client_order_id; // Empty when the order was not placed by us.
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double filled = 0.0;
    double unfilled = 0.0;
};

struct VenueFill {
    std::string venue_fill_id;
    std::string venue_order_id;
    std::string client_order_id; // Empty for fills of orders not placed by us (e.g. liquidation).
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;       // Underlying units.
    double price = 0.0;          // USD per underlying unit.
    Timestamp day = 0;           // UTC business day of the fill (YYYYMMDD).
    long long fill_time_ms = 0;  // Unix milliseconds, to order fills and detect reused client ids.
    std::string fill_time;       // Venue timestamp, kept for the journal.
    std::string fill_type;       // Venue classification, e.g. "taker", "liquidation".
};

struct Position {
    std::string symbol;
    double signed_quantity = 0.0; // Underlying units; negative for short.
};

struct AccountState {
    // USD balance of the trading wallet. Collateral valuation (for example BTC backing)
    // is reported separately by the account monitor; see roadmap step 8.
    double usd_balance = 0.0;
    std::vector<Position> positions;
};

struct RateBudget {
    int capacity = 0;     // Cost units available in one window.
    int window_ms = 0;    // Refill period for the full capacity.
};

// ------------------------------------------------------------- per-venue part
class VenueProtocol {
public:
    virtual ~VenueProtocol() = default;

    virtual std::string name() const = 0;            // Stable id, e.g. "kraken-futures".
    virtual RateBudget rateBudget() const = 0;
    virtual int cost(Endpoint endpoint) const = 0;   // Rate cost of one request.

    // Client order ids must be unique per account and recognisable as ours.
    virtual std::string clientOrderId(const std::string& accountNamespace, OrderID localOrderId) const = 0;
    virtual bool isOurClientOrderId(const std::string& accountNamespace, const std::string& clientOrderId) const = 0;

    virtual HttpRequest permissionsRequest() = 0;
    virtual Permissions parsePermissions(const HttpResponse& response) const = 0;

    virtual HttpRequest instrumentsRequest() = 0;
    virtual std::vector<Market> parseMarkets(const HttpResponse& response) const = 0;

    // Current mark prices by venue symbol, in USD per venue unit.
    virtual HttpRequest tickersRequest() = 0;
    virtual std::map<std::string, double> parseMarkPrices(const HttpResponse& response) const = 0;

    virtual HttpRequest submitRequest(const OrderRequest& order) = 0;
    virtual SubmitResult parseSubmit(const HttpResponse& response) const = 0;

    virtual HttpRequest cancelRequest(const std::string& clientOrderId) = 0;
    virtual CancelResult parseCancel(const HttpResponse& response) const = 0;

    virtual HttpRequest openOrdersRequest() = 0;
    virtual std::vector<OpenOrder> parseOpenOrders(const HttpResponse& response) const = 0;

    virtual HttpRequest fillsRequest() = 0; // Most recent fills, newest first.
    virtual std::vector<VenueFill> parseFills(const HttpResponse& response) const = 0;

    virtual HttpRequest accountsRequest() = 0;
    virtual HttpRequest positionsRequest() = 0;
    virtual AccountState parseAccount(const HttpResponse& accounts, const HttpResponse& positions) const = 0;

    // Venue-side cancel-on-silence timer. Empty when the venue has none.
    virtual std::optional<HttpRequest> deadMansSwitchRequest(int timeoutSeconds) = 0;
};

// Raised by protocols when a response cannot be interpreted. The message is our own
// wording (endpoint and field), never the response body.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace Venue
