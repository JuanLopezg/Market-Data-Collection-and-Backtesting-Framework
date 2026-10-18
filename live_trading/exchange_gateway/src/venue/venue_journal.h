#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "data_types.h"
#include "execution_order.h"
#include "venue_protocol.h"

struct sqlite3;

// Durable local record of every order and fill handled by a real venue adapter.
//
// Order of operations (crash contract):
//   1. an order is journaled PREPARED before the gateway acknowledges the NATS command;
//   2. it is marked SENDING (committed) before the HTTP request leaves the process;
//   3. the venue answer is committed before any event is published;
//   4. a venue fill gets its local FillID in the same transaction that updates the order.
// After a crash, SENDING means "the venue may have the order": it becomes UNKNOWN and is
// resolved from venue evidence (open orders, fills), never by resending.
namespace Venue {

enum class OrderState {
    Prepared,  // Journaled; not sent (waiting for active_from or for the next poll).
    Sending,   // Request committed as in flight.
    Open,      // Venue accepted the order.
    Unknown,   // Outcome not known; needs venue evidence.
    Filled,
    Cancelled, // Includes partially filled orders whose remainder ended.
    Rejected   // Refused by the venue or locally (submission disabled, market unavailable, ...).
};

const char* orderStateName(OrderState state);
bool isTerminal(OrderState state);

struct JournalOrder {
    OrderID local_order_id = 0;
    StrategyID strategy_id = 0;
    std::string client_order_id;
    Coin coin;
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double source_quantity = 0.0; // As requested, in source (Binance) units.
    double venue_quantity = 0.0;  // Rounded, in underlying units.
    double unit_scale = 1.0;      // Underlying units per source unit.
    Timestamp active_from = 0;
    OrderState state = OrderState::Prepared;
    std::string venue_order_id;
    double filled_venue_quantity = 0.0;
    std::string reason;
    bool cancel_requested = false;
    long long sent_at_ms = 0;
    long long updated_at_ms = 0;
    std::optional<OrderState> published_state; // Last state reported to the gateway.
    VenueOrderType type = VenueOrderType::Market;
    double position_stop_price = 0.0; // Original command, USD per source unit.
    double venue_stop_price = 0.0;    // Rounded down to the venue tick, USD per venue unit.
};

struct JournalFill {
    std::string venue_fill_id;
    FillID local_fill_id = 0;
    OrderID local_order_id = 0;
    double venue_quantity = 0.0;
    double price = 0.0;
    Timestamp day = 0;
    bool published = false;
};

// A USD cash entry from the venue's account history (fee, funding, interest or capital
// flow), as published to execution.
struct JournalCashEntry {
    std::string entry_id;
    VenueCostKind kind = VenueCostKind::Fee;
    Timestamp day = 0;
    Coin coin;                 // Source symbol, or the venue symbol when it is not mapped.
    FillID local_fill_id = 0;  // The fee's fill when it is one of ours.
    double amount_usd = 0.0;   // Negative = paid.
    bool published = false;
};

class VenueJournal {
public:
    // Binds the file to one venue and account namespace; a mismatch is refused.
    VenueJournal(const std::string& path, const std::string& venue, const std::string& accountNamespace);
    ~VenueJournal();
    VenueJournal(const VenueJournal&) = delete;
    VenueJournal& operator=(const VenueJournal&) = delete;

    std::optional<JournalOrder> order(OrderID localOrderId) const;
    std::optional<JournalOrder> orderByClientId(const std::string& clientOrderId) const;
    std::optional<JournalOrder> orderByVenueId(const std::string& venueOrderId) const;
    std::vector<JournalOrder> ordersIn(std::initializer_list<OrderState> states) const;
    std::vector<JournalOrder> unpublishedOrders() const;

    void insertOrder(const JournalOrder& order);
    void updateOrder(const JournalOrder& order);
    void markOrderPublished(OrderID localOrderId, OrderState state);

    bool knowsFill(const std::string& venueFillId) const;
    // Allocates the next local FillID and stores the fill with the updated order atomically.
    JournalFill recordFill(const VenueFill& fill, const JournalOrder& updatedOrder);
    std::vector<JournalFill> unpublishedFills() const;
    void markFillPublished(const std::string& venueFillId);

    // Fills on our account that no order of ours explains (manual trades, liquidations).
    // Returns true the first time a fill id is recorded.
    bool recordUnattributedFill(const VenueFill& fill);

    // The local fill (and its order id) recorded for a venue fill id, when it is ours.
    std::optional<JournalFill> fillByVenueId(const std::string& venueFillId) const;

    // Account history. The first read only marks what already existed (baseline); every
    // entry seen after that is recorded once and published once.
    bool accountLogBaselined() const;
    void markAccountLogBaselined();
    bool knowsCashEntry(const std::string& entryId) const;
    void recordCashEntry(const JournalCashEntry& entry);
    std::vector<JournalCashEntry> unpublishedCashEntries() const;
    void markCashEntryPublished(const std::string& entryId);

private:
    sqlite3* db_ = nullptr;

    void execute(const char* sql) const;
    std::vector<JournalOrder> selectOrders(const std::string& where) const;
};

} // namespace Venue
