#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>

#include "exchange_gateway_adapter.h"
#include "symbol_map.h"
#include "venue_journal.h"
#include "venue_protocol.h"

// Common real-venue adapter behind the exchange gateway.
//
// Everything here is venue-neutral; VenueProtocol supplies the exchange-specific parts.
//
// Safety rules:
// - order submission is disabled unless explicitly enabled; then every order is rejected
//   locally and only GET requests can leave the process (GetOnlyTransport);
// - an order is journaled before the gateway acknowledges its command;
// - an ambiguous submit becomes UNKNOWN and is resolved only from venue evidence
//   (open orders, fills); it is never resent automatically;
// - fills are de-duplicated by venue fill id and get a stable local FillID;
// - fees and funding come from the venue's account history, each entry published once;
// - the snapshot reports cash as on a spot account (USD balance minus the entry value of
//   open positions), the convention execution-state keeps, so reconciliation compares like
//   with like;
// - unavailable markets are rejected with a reason; no other product is substituted;
// - source symbols and units go through the explicit SymbolMap only;
// - before an order is journaled its USD value at the venue (venue quantity x venue mark
//   price) must agree with the planned value (source quantity x reference price), so a
//   wrong unit scale cannot send an order 1000 times too large or too small;
// - technical time (polling, rate budget) never becomes business time.
namespace Venue {

// Host wall clock in Unix milliseconds for technical work only: polling intervals,
// rate budget and comparison with venue timestamps. Never business time.
long long technicalUnixMs();

struct LiveVenueOptions {
    bool order_submission_enabled = false;
    std::string account_namespace;                 // Short tag in client order ids, e.g. "a1".
    long long working_poll_interval_ms = 2000;     // Fill/open-order checks while orders work.
    long long market_refresh_interval_ms = 3600000;
    long long mark_price_max_age_ms = 60000;       // Venue prices older than this are fetched again.
    double max_notional_ratio = 1.5;               // Venue/planned USD value allowed in [1/x, x].
    long long settle_after_ms = 15000;             // Absent from open orders this long: remainder ended.
    long long unknown_alert_after_ms = 120000;     // UNKNOWN this long: ALERT for the operator.
    int dead_mans_switch_seconds = 0;              // 0: not armed.
    long long account_log_interval_ms = 300000;    // Fees and funding from the account history.
    long long fee_lookup_after_fill_ms = 30000;    // Read it again this soon after a new fill.
};

class LiveVenueAdapter final : public ExchangeGatewayAdapter {
public:
    using UnixMsClock = std::function<long long()>;  // Technical time only.
    using BusinessDay = std::function<Timestamp()>;  // TimeHandler business date (YYYYMMDD).

    LiveVenueAdapter(
        VenueProtocol& protocol,
        HttpTransport& transport,
        VenueJournal& journal,
        SymbolMap symbolMap,
        BusinessDay businessDay,
        UnixMsClock nowMs,
        LiveVenueOptions options
    );

    // Reads the key permissions. Refuses keys with withdrawal access, and refuses an
    // enabled submission configuration whose key cannot trade.
    Permissions verifyPermissions();

    void setHandlers(ExchangeGatewayHandlers handlers) override;
    void submitOrder(const SubmitOrderCommand& command) override;
    void cancelOrder(const CancelOrderCommand& command) override;
    void requestSnapshot(const ExchangeSnapshotRequest& request) override;
    void poll(int timeoutMs) override;

private:
    // Token bucket with the venue's documented budget.
    class RateLimiter {
    public:
        explicit RateLimiter(RateBudget budget);
        bool tryTake(int cost, long long nowMs);

    private:
        RateBudget budget_;
        double available_;
        long long last_ms_ = 0;
    };

    VenueProtocol& protocol_;
    HttpTransport& transport_;
    VenueJournal& journal_;
    SymbolMap symbol_map_;
    BusinessDay business_day_;
    UnixMsClock now_ms_;
    LiveVenueOptions options_;
    ExchangeGatewayHandlers handlers_;
    RateLimiter rate_limiter_;

    std::map<std::string, Market> markets_by_symbol_;
    long long markets_loaded_at_ms_ = 0;
    std::map<std::string, double> mark_prices_;  // By venue symbol, USD per venue unit.
    long long mark_prices_loaded_at_ms_ = 0;

    long long started_at_ms_ = 0; // Fills before this are history, not unexplained new trades.
    bool recovered_ = false;
    long long last_venue_check_ms_ = 0;
    long long last_dead_mans_switch_ms_ = 0;
    std::map<OrderID, long long> last_cancel_attempt_ms_;
    std::map<OrderID, long long> last_evidence_ms_;
    std::set<OrderID> unknown_alerted_;
    long long next_account_log_ms_ = 0;          // 0: read at the first poll.
    std::set<std::string> unaccounted_logged_;   // History kinds already reported once.

    HttpResponse send(const HttpRequest& request, Endpoint endpoint);
    void loadMarkets();
    void loadMarketsIfDue();
    void loadMarkPricesIfDue();

    void rejectLocally(JournalOrder order, const std::string& reason);
    void recoverInterruptedSends();
    void sendDueOrders();
    void sendOrder(JournalOrder order);
    void sendCancels();
    void checkVenue();
    void applyFills(const std::vector<VenueFill>& fills);
    void applyOpenOrders(const std::vector<OpenOrder>& openOrders);
    void refreshDeadMansSwitch();
    void readAccountLogIfDue();

    void publishPending();
    void publishCashEntry(const JournalCashEntry& entry);
    void publishOrder(const JournalOrder& order);
    void publishFill(const JournalFill& fill, const JournalOrder& order);
    void transition(JournalOrder& order, OrderState state, const std::string& reason);
};

} // namespace Venue
