#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "fill.h"


// One account-level position on one asset, from flat to flat (all strategies together).
//
// Rules (the same as the dashboard's former rebuild, so reports do not change):
// adding to a position averages its entry price; reducing it realizes
// (exit - entry) x quantity at the average entry (sign flipped for shorts); a fill that
// crosses zero closes the position and opens the opposite one with the remainder, its
// fee split by quantity. Funding goes to the position open on that asset at that moment
// (positive = received). Net PnL = gross PnL - fees + funding.
struct PositionLot {
    Coin coin;
    bool long_side = true;
    Timestamp opened_at = 0;
    Timestamp closed_at = 0;        // 0 while open
    FillID closing_fill_id = 0;     // fill that brought it back to flat; 0 while open

    double quantity = 0.0;          // signed size still open
    double max_quantity = 0.0;      // largest absolute size reached
    double entry_price = 0.0;       // average entry price
    double entry_value = 0.0;       // sum of entry quantity x price (cost basis)
    double exit_value = 0.0;        // sum of exit quantity x price (proceeds)
    double exit_quantity = 0.0;
    double gross_pnl = 0.0;         // realized so far
    double fees = 0.0;
    double funding = 0.0;

    double netPnl() const { return gross_pnl - fees + funding; }
    double exitPrice() const { return exit_quantity > 0.0 ? exit_value / exit_quantity : 0.0; }
    const char* sideName() const { return long_side ? "Long" : "Short"; }
};


// A perpetual funding payment on an asset (positive = received). Recorded only for a real
// venue; the PAPER simulation has none.
struct FundingPayment {
    std::string payment_id;
    Timestamp timestamp = 0;
    Coin coin;
    double amount = 0.0;
};


// Cost basis, realized PnL, fees and funding per position, owned by the runtime and
// rebuilt deterministically from the fill audit trail (plus funding) on restart.
class PositionLedger {
private:
    std::map<Coin, PositionLot> open_;
    std::vector<PositionLot> closed_;
    std::unordered_set<FillID> applied_fills_;
    std::unordered_set<std::string> applied_funding_;

    void openLot(const Fill& fill, double quantity, double fee);

public:
    // Applies a fill once (a repeated fill id is ignored). Returns the position it closed,
    // if any.
    std::optional<PositionLot> applyFill(const Fill& fill);

    // Applies a funding payment once (a repeated payment id is ignored). A payment with no
    // open position on that asset has nothing to attach to and is ignored.
    void applyFunding(const FundingPayment& payment);

    // Fills in (timestamp, fill_id) order; funding at a timestamp belongs to the position
    // held up to it, so it is applied before that timestamp's fills.
    static PositionLedger rebuild(std::vector<Fill> fills, std::vector<FundingPayment> funding);

    const std::map<Coin, PositionLot>& openPositions() const { return open_; }
    const std::vector<PositionLot>& closedPositions() const { return closed_; }

    // Totals over closed and open positions.
    double realizedGrossPnl() const;
    double fees() const;
    double funding() const;
};
