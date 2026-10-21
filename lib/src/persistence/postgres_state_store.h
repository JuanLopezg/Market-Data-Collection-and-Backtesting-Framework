#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "execution_messages.h"
#include "position_ledger.h"
#include "state_store.h"


// Account value marked at one daily close (all amounts in the settlement currency).
struct EquityDay {
    Timestamp date = 0;              // YYYYMMDD of the close used for marking
    double cash = 0.0;
    double positions_value = 0.0;    // sum of signed quantity x close
    double equity = 0.0;             // cash + positions_value
    double realized_gross_pnl = 0.0; // ledger totals at that close
    double fees = 0.0;
    double funding = 0.0;
    std::size_t open_positions = 0;
    // Real venues only (0 in PAPER), totals up to that close:
    double capital_flows = 0.0;      // net USD deposited (+) / withdrawn (-), not performance
    double interest = 0.0;           // venue interest, negative = paid
    double collateral_usd = 0.0;     // venue collateral (BTC) after haircut used that day; not in equity
    // Signed entry value of the positions open at that close (quantity x average entry):
    // cash + open_cost is the balance without unrealized PnL (a perpetual venue's USD balance).
    double open_cost = 0.0;
};


// Distributed-runtime StateStore backed by PostgreSQL
//
// The operational snapshot is stored as one JSONB document and the fill audit trail is
// append-only. save(snapshot, fill) commits both inside one PostgreSQL transaction.
//
// The accounting tables (open/closed positions, daily equity) are a projection the
// runtime writes from PositionLedger; they can always be rebuilt from trading_fills plus
// trading_funding, so they are written after the fill commit, not inside it.
class PostgresStateStore final : public StateStore {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit PostgresStateStore(const std::string& connectionString);
    ~PostgresStateStore() override;

    PostgresStateStore(const PostgresStateStore&) = delete;
    PostgresStateStore& operator=(const PostgresStateStore&) = delete;

    void save(
        const TradingStateSnapshot& snapshot,
        const std::optional<Fill>& newFill = std::nullopt
    ) override;

    std::optional<TradingStateSnapshot> load() const override;
    std::vector<Fill> loadFills() const override;

    // Replace every open/closed position row with the ledger's (startup rebuild).
    void replaceLedger(const PositionLedger& ledger);
    // After a fill: rewrite the open positions and add the positions it closed.
    void saveLedger(const PositionLedger& ledger, const std::vector<PositionLot>& newlyClosed);
    // A day's equity row is written once: the first write wins and later ones (a market
    // update redelivered after a restart, when the account already holds the next open's
    // fills) are ignored. Returns whether this call wrote the row.
    bool insertEquityDay(const EquityDay& day);
    std::set<Timestamp> equityDays() const;
    // Rows written before open_cost existed (2026-10-21) have it NULL; the backfill fills in
    // only that column, never another one of a written row.
    std::set<Timestamp> equityDaysWithOpenCost() const;
    void fillMissingOpenCost(Timestamp day, double openCost);
    std::vector<FundingPayment> loadFunding() const;

    // Venue cash movements (fees after the fill, funding). saveVenueCost commits the cost
    // row and the snapshot already holding its cash together; it throws std::logic_error
    // when the cost was applied before (nothing is written then).
    bool knowsVenueCost(const std::string& costId) const;
    void saveVenueCost(const TradingStateSnapshot& snapshot, const VenueCostEvent& cost);
    // Venue fees per local fill (positive = paid), added to the fill commissions.
    std::unordered_map<FillID, double> loadFillFees() const;
    // Account-level venue movements (interest and capital flows), oldest first.
    std::vector<VenueCostEvent> loadAccountCashMovements() const;

    // Venue collateral used for one day's sizing snapshot. The first call for a day stores
    // `current`; later calls return the stored value, so a redelivered daily snapshot is
    // identical.
    double fixDailyCollateral(Timestamp day, double current);
    std::optional<double> dailyCollateral(Timestamp day) const;

    // Close loop: result of every reconciliation against the exchange on one UTC day.
    // A day stays BLOCKED once any check of that day found issues, even if a later check
    // is clean, so the dashboard shows that the loop broke that day.
    void recordReconciliation(Timestamp day, bool clean, std::size_t issues);
};
