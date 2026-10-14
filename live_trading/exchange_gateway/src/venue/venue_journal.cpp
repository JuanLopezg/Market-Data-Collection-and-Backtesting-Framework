#include "venue_journal.h"

#include <stdexcept>

#include <sqlite3.h>

namespace Venue {

namespace {

constexpr OrderState ALL_STATES[] = {
    OrderState::Prepared, OrderState::Sending, OrderState::Open, OrderState::Unknown,
    OrderState::Filled, OrderState::Cancelled, OrderState::Rejected
};

OrderState parseState(const std::string& text)
{
    for (const OrderState state : ALL_STATES) {
        if (text == orderStateName(state))
            return state;
    }
    throw std::runtime_error("Venue journal contains an unknown order state: " + text);
}

// Prepared statement with automatic finalization.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("Venue journal statement failed: ") + sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value)
    {
        check(sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT));
    }
    void bind(int index, long long value) { check(sqlite3_bind_int64(statement_, index, value)); }
    void bind(int index, double value) { check(sqlite3_bind_double(statement_, index, value)); }
    void bindNull(int index) { check(sqlite3_bind_null(statement_, index)); }

    bool step()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        throw std::runtime_error(std::string("Venue journal write/read failed: ") + sqlite3_errmsg(db_));
    }

    std::string text(int column) const
    {
        const unsigned char* value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
    }
    long long integer(int column) const { return sqlite3_column_int64(statement_, column); }
    double real(int column) const { return sqlite3_column_double(statement_, column); }
    bool isNull(int column) const { return sqlite3_column_type(statement_, column) == SQLITE_NULL; }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;

    void check(int result)
    {
        if (result != SQLITE_OK)
            throw std::runtime_error(std::string("Venue journal bind failed: ") + sqlite3_errmsg(db_));
    }
};

// One transaction; rolls back unless commit() was reached.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { run("BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit()
    {
        run("COMMIT");
        committed_ = true;
    }

private:
    sqlite3* db_;
    bool committed_ = false;

    void run(const char* sql)
    {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("Venue journal transaction failed: ") + sqlite3_errmsg(db_));
    }
};

const char* ORDER_COLUMNS =
    "local_order_id, strategy_id, client_order_id, coin, symbol, side, source_quantity, venue_quantity,"
    " unit_scale, active_from, state, venue_order_id, filled_venue_quantity, reason, cancel_requested,"
    " sent_at_ms, updated_at_ms, published_state";

JournalOrder readOrder(const Statement& row)
{
    JournalOrder order;
    order.local_order_id = static_cast<OrderID>(row.integer(0));
    order.strategy_id = static_cast<StrategyID>(row.integer(1));
    order.client_order_id = row.text(2);
    order.coin = row.text(3);
    order.symbol = row.text(4);
    order.side = row.integer(5) == 0 ? OrderSide::Buy : OrderSide::Sell;
    order.source_quantity = row.real(6);
    order.venue_quantity = row.real(7);
    order.unit_scale = row.real(8);
    order.active_from = static_cast<Timestamp>(row.integer(9));
    order.state = parseState(row.text(10));
    order.venue_order_id = row.text(11);
    order.filled_venue_quantity = row.real(12);
    order.reason = row.text(13);
    order.cancel_requested = row.integer(14) != 0;
    order.sent_at_ms = row.integer(15);
    order.updated_at_ms = row.integer(16);
    if (!row.isNull(17))
        order.published_state = parseState(row.text(17));
    return order;
}

void bindOrder(Statement& statement, const JournalOrder& order)
{
    statement.bind(1, static_cast<long long>(order.local_order_id));
    statement.bind(2, static_cast<long long>(order.strategy_id));
    statement.bind(3, order.client_order_id);
    statement.bind(4, order.coin);
    statement.bind(5, order.symbol);
    statement.bind(6, static_cast<long long>(order.side == OrderSide::Buy ? 0 : 1));
    statement.bind(7, order.source_quantity);
    statement.bind(8, order.venue_quantity);
    statement.bind(9, order.unit_scale);
    statement.bind(10, static_cast<long long>(order.active_from));
    statement.bind(11, std::string(orderStateName(order.state)));
    statement.bind(12, order.venue_order_id);
    statement.bind(13, order.filled_venue_quantity);
    statement.bind(14, order.reason);
    statement.bind(15, static_cast<long long>(order.cancel_requested ? 1 : 0));
    statement.bind(16, order.sent_at_ms);
    statement.bind(17, order.updated_at_ms);
    if (order.published_state)
        statement.bind(18, std::string(orderStateName(*order.published_state)));
    else
        statement.bindNull(18);
}

} // namespace


const char* orderStateName(OrderState state)
{
    switch (state) {
        case OrderState::Prepared:  return "PREPARED";
        case OrderState::Sending:   return "SENDING";
        case OrderState::Open:      return "OPEN";
        case OrderState::Unknown:   return "UNKNOWN";
        case OrderState::Filled:    return "FILLED";
        case OrderState::Cancelled: return "CANCELLED";
        case OrderState::Rejected:  return "REJECTED";
    }
    return "INVALID";
}


bool isTerminal(OrderState state)
{
    return state == OrderState::Filled || state == OrderState::Cancelled || state == OrderState::Rejected;
}


VenueJournal::VenueJournal(const std::string& path, const std::string& venue, const std::string& accountNamespace)
{
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string error = db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Cannot open venue journal " + path + ": " + error);
    }
    execute("PRAGMA journal_mode=WAL");
    execute("PRAGMA synchronous=FULL");
    execute("PRAGMA foreign_keys=ON");
    execute(
        "CREATE TABLE IF NOT EXISTS context ("
        " id INTEGER PRIMARY KEY CHECK (id = 1), venue TEXT NOT NULL, account_namespace TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS orders ("
        " local_order_id INTEGER PRIMARY KEY, strategy_id INTEGER NOT NULL, client_order_id TEXT NOT NULL UNIQUE,"
        " coin TEXT NOT NULL, symbol TEXT NOT NULL, side INTEGER NOT NULL, source_quantity REAL NOT NULL,"
        " venue_quantity REAL NOT NULL, unit_scale REAL NOT NULL, active_from INTEGER NOT NULL,"
        " state TEXT NOT NULL, venue_order_id TEXT NOT NULL, filled_venue_quantity REAL NOT NULL,"
        " reason TEXT NOT NULL, cancel_requested INTEGER NOT NULL, sent_at_ms INTEGER NOT NULL,"
        " updated_at_ms INTEGER NOT NULL, published_state TEXT);"
        "CREATE TABLE IF NOT EXISTS fills ("
        " venue_fill_id TEXT PRIMARY KEY, local_fill_id INTEGER NOT NULL UNIQUE,"
        " local_order_id INTEGER NOT NULL REFERENCES orders(local_order_id),"
        " venue_quantity REAL NOT NULL, price REAL NOT NULL, day INTEGER NOT NULL,"
        " fill_time TEXT NOT NULL, fill_type TEXT NOT NULL, published INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS unattributed_fills ("
        " venue_fill_id TEXT PRIMARY KEY, symbol TEXT NOT NULL, side INTEGER NOT NULL,"
        " venue_quantity REAL NOT NULL, price REAL NOT NULL, fill_time TEXT NOT NULL, fill_type TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS cash_entries ("
        " entry_id TEXT PRIMARY KEY, kind TEXT NOT NULL, day INTEGER NOT NULL, coin TEXT NOT NULL,"
        " local_fill_id INTEGER NOT NULL, amount_usd REAL NOT NULL, published INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS settings (name TEXT PRIMARY KEY, value TEXT NOT NULL);"
    );

    Statement existing(db_, "SELECT venue, account_namespace FROM context WHERE id = 1");
    if (existing.step()) {
        if (existing.text(0) != venue || existing.text(1) != accountNamespace)
            throw std::runtime_error("Venue journal " + path + " belongs to another venue or account namespace");
        return;
    }
    Statement insert(db_, "INSERT INTO context (id, venue, account_namespace) VALUES (1, ?, ?)");
    insert.bind(1, venue);
    insert.bind(2, accountNamespace);
    insert.step();
}


VenueJournal::~VenueJournal()
{
    sqlite3_close(db_);
}


void VenueJournal::execute(const char* sql) const
{
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw std::runtime_error("Venue journal setup failed: " + message);
    }
}


std::vector<JournalOrder> VenueJournal::selectOrders(const std::string& where) const
{
    const std::string sql = std::string("SELECT ") + ORDER_COLUMNS + " FROM orders " + where +
                            " ORDER BY local_order_id";
    Statement statement(db_, sql.c_str());
    std::vector<JournalOrder> result;
    while (statement.step())
        result.push_back(readOrder(statement));
    return result;
}


std::optional<JournalOrder> VenueJournal::order(OrderID localOrderId) const
{
    auto rows = selectOrders("WHERE local_order_id = " + std::to_string(localOrderId));
    if (rows.empty())
        return std::nullopt;
    return rows.front();
}


std::optional<JournalOrder> VenueJournal::orderByClientId(const std::string& clientOrderId) const
{
    const std::string sql = std::string("SELECT ") + ORDER_COLUMNS + " FROM orders WHERE client_order_id = ?";
    Statement statement(db_, sql.c_str());
    statement.bind(1, clientOrderId);
    if (!statement.step())
        return std::nullopt;
    return readOrder(statement);
}


std::vector<JournalOrder> VenueJournal::ordersIn(std::initializer_list<OrderState> states) const
{
    std::string list;
    for (const OrderState state : states)
        list += std::string(list.empty() ? "" : ",") + "'" + orderStateName(state) + "'";
    return selectOrders("WHERE state IN (" + list + ")");
}


std::vector<JournalOrder> VenueJournal::unpublishedOrders() const
{
    return selectOrders("WHERE published_state IS NULL OR published_state <> state");
}


void VenueJournal::insertOrder(const JournalOrder& order)
{
    const std::string sql = std::string("INSERT INTO orders (") + ORDER_COLUMNS +
                            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    Statement statement(db_, sql.c_str());
    bindOrder(statement, order);
    statement.step();
}


void VenueJournal::updateOrder(const JournalOrder& order)
{
    Statement statement(db_,
        "UPDATE orders SET strategy_id = ?2, client_order_id = ?3, coin = ?4, symbol = ?5, side = ?6,"
        " source_quantity = ?7, venue_quantity = ?8, unit_scale = ?9, active_from = ?10, state = ?11,"
        " venue_order_id = ?12, filled_venue_quantity = ?13, reason = ?14, cancel_requested = ?15,"
        " sent_at_ms = ?16, updated_at_ms = ?17, published_state = ?18 WHERE local_order_id = ?1");
    bindOrder(statement, order);
    statement.step();
    if (sqlite3_changes(db_) != 1)
        throw std::runtime_error("Venue journal has no order " + std::to_string(order.local_order_id));
}


void VenueJournal::markOrderPublished(OrderID localOrderId, OrderState state)
{
    Statement statement(db_, "UPDATE orders SET published_state = ? WHERE local_order_id = ?");
    statement.bind(1, std::string(orderStateName(state)));
    statement.bind(2, static_cast<long long>(localOrderId));
    statement.step();
}


bool VenueJournal::knowsFill(const std::string& venueFillId) const
{
    Statement statement(db_,
        "SELECT 1 FROM fills WHERE venue_fill_id = ?1 UNION ALL SELECT 1 FROM unattributed_fills WHERE venue_fill_id = ?1");
    statement.bind(1, venueFillId);
    return statement.step();
}


JournalFill VenueJournal::recordFill(const VenueFill& fill, const JournalOrder& updatedOrder)
{
    Transaction transaction(db_);

    Statement next(db_, "SELECT COALESCE(MAX(local_fill_id), 0) + 1 FROM fills");
    next.step();
    JournalFill stored;
    stored.venue_fill_id = fill.venue_fill_id;
    stored.local_fill_id = static_cast<FillID>(next.integer(0));
    stored.local_order_id = updatedOrder.local_order_id;
    stored.venue_quantity = fill.quantity;
    stored.price = fill.price;
    stored.day = fill.day;

    Statement insert(db_,
        "INSERT INTO fills (venue_fill_id, local_fill_id, local_order_id, venue_quantity, price, day,"
        " fill_time, fill_type, published) VALUES (?,?,?,?,?,?,?,?,0)");
    insert.bind(1, stored.venue_fill_id);
    insert.bind(2, static_cast<long long>(stored.local_fill_id));
    insert.bind(3, static_cast<long long>(stored.local_order_id));
    insert.bind(4, stored.venue_quantity);
    insert.bind(5, stored.price);
    insert.bind(6, static_cast<long long>(stored.day));
    insert.bind(7, fill.fill_time);
    insert.bind(8, fill.fill_type);
    insert.step();

    updateOrder(updatedOrder);
    transaction.commit();
    return stored;
}


std::vector<JournalFill> VenueJournal::unpublishedFills() const
{
    Statement statement(db_,
        "SELECT venue_fill_id, local_fill_id, local_order_id, venue_quantity, price, day FROM fills"
        " WHERE published = 0 ORDER BY local_fill_id");
    std::vector<JournalFill> result;
    while (statement.step()) {
        JournalFill fill;
        fill.venue_fill_id = statement.text(0);
        fill.local_fill_id = static_cast<FillID>(statement.integer(1));
        fill.local_order_id = static_cast<OrderID>(statement.integer(2));
        fill.venue_quantity = statement.real(3);
        fill.price = statement.real(4);
        fill.day = static_cast<Timestamp>(statement.integer(5));
        result.push_back(fill);
    }
    return result;
}


void VenueJournal::markFillPublished(const std::string& venueFillId)
{
    Statement statement(db_, "UPDATE fills SET published = 1 WHERE venue_fill_id = ?");
    statement.bind(1, venueFillId);
    statement.step();
}


bool VenueJournal::recordUnattributedFill(const VenueFill& fill)
{
    Statement statement(db_,
        "INSERT OR IGNORE INTO unattributed_fills (venue_fill_id, symbol, side, venue_quantity, price,"
        " fill_time, fill_type) VALUES (?,?,?,?,?,?,?)");
    statement.bind(1, fill.venue_fill_id);
    statement.bind(2, fill.symbol);
    statement.bind(3, static_cast<long long>(fill.side == OrderSide::Buy ? 0 : 1));
    statement.bind(4, fill.quantity);
    statement.bind(5, fill.price);
    statement.bind(6, fill.fill_time);
    statement.bind(7, fill.fill_type);
    statement.step();
    return sqlite3_changes(db_) == 1;
}


std::optional<JournalFill> VenueJournal::fillByVenueId(const std::string& venueFillId) const
{
    Statement statement(db_,
        "SELECT venue_fill_id, local_fill_id, local_order_id, venue_quantity, price, day, published FROM fills"
        " WHERE venue_fill_id = ?");
    statement.bind(1, venueFillId);
    if (!statement.step())
        return std::nullopt;
    JournalFill fill;
    fill.venue_fill_id = statement.text(0);
    fill.local_fill_id = static_cast<FillID>(statement.integer(1));
    fill.local_order_id = static_cast<OrderID>(statement.integer(2));
    fill.venue_quantity = statement.real(3);
    fill.price = statement.real(4);
    fill.day = static_cast<Timestamp>(statement.integer(5));
    fill.published = statement.integer(6) != 0;
    return fill;
}


bool VenueJournal::accountLogBaselined() const
{
    Statement statement(db_, "SELECT 1 FROM settings WHERE name = 'account_log_baseline'");
    return statement.step();
}


void VenueJournal::markAccountLogBaselined()
{
    execute("INSERT OR IGNORE INTO settings (name, value) VALUES ('account_log_baseline', 'done')");
}


bool VenueJournal::knowsCashEntry(const std::string& entryId) const
{
    Statement statement(db_, "SELECT 1 FROM cash_entries WHERE entry_id = ?");
    statement.bind(1, entryId);
    return statement.step();
}


void VenueJournal::recordCashEntry(const JournalCashEntry& entry)
{
    Statement statement(db_,
        "INSERT OR IGNORE INTO cash_entries (entry_id, kind, day, coin, local_fill_id, amount_usd, published)"
        " VALUES (?,?,?,?,?,?,?)");
    statement.bind(1, entry.entry_id);
    statement.bind(2, std::string(venueCostKindName(entry.kind)));
    statement.bind(3, static_cast<long long>(entry.day));
    statement.bind(4, entry.coin);
    statement.bind(5, static_cast<long long>(entry.local_fill_id));
    statement.bind(6, entry.amount_usd);
    statement.bind(7, static_cast<long long>(entry.published ? 1 : 0));
    statement.step();
}


std::vector<JournalCashEntry> VenueJournal::unpublishedCashEntries() const
{
    Statement statement(db_,
        "SELECT entry_id, kind, day, coin, local_fill_id, amount_usd FROM cash_entries WHERE published = 0"
        " ORDER BY day, entry_id");
    std::vector<JournalCashEntry> result;
    while (statement.step()) {
        JournalCashEntry entry;
        entry.entry_id = statement.text(0);
        const std::optional<VenueCostKind> kind = venueCostKindFromName(statement.text(1));
        if (!kind)
            throw std::runtime_error("Venue journal cash entry " + entry.entry_id + " has an unknown kind");
        entry.kind = *kind;
        entry.day = static_cast<Timestamp>(statement.integer(2));
        entry.coin = statement.text(3);
        entry.local_fill_id = static_cast<FillID>(statement.integer(4));
        entry.amount_usd = statement.real(5);
        result.push_back(entry);
    }
    return result;
}


void VenueJournal::markCashEntryPublished(const std::string& entryId)
{
    Statement statement(db_, "UPDATE cash_entries SET published = 1 WHERE entry_id = ?");
    statement.bind(1, entryId);
    statement.step();
}

} // namespace Venue
