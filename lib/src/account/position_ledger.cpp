#include "position_ledger.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>


namespace {

// Treats float residue from summing fills as zero (relative to the position size).
double quantityEpsilon(double reference)
{
    return 1e-9 * std::max(1.0, std::abs(reference));
}

}


void PositionLedger::openLot(const Fill& fill, double quantity, double fee)
{
    PositionLot lot;
    lot.coin = fill.coin;
    lot.long_side = fill.side == OrderSide::Buy;
    lot.opened_at = fill.timestamp;
    lot.quantity = lot.long_side ? quantity : -quantity;
    lot.max_quantity = quantity;
    lot.entry_price = fill.price;
    lot.entry_value = quantity * fill.price;
    lot.fees = fee;
    open_[fill.coin] = lot;
}


std::optional<PositionLot> PositionLedger::applyFill(const Fill& fill)
{
    fill.validate();
    if (!applied_fills_.insert(fill.fill_id).second)
        return std::nullopt;

    const auto found = open_.find(fill.coin);
    if (found == open_.end()) {
        openLot(fill, fill.quantity, fill.commission);
        return std::nullopt;
    }

    PositionLot& lot = found->second;
    const bool buy = fill.side == OrderSide::Buy;
    if ((lot.quantity > 0.0) == buy) {
        const double size = std::abs(lot.quantity);
        lot.entry_price = (size * lot.entry_price + fill.quantity * fill.price) / (size + fill.quantity);
        lot.entry_value += fill.quantity * fill.price;
        lot.quantity += buy ? fill.quantity : -fill.quantity;
        lot.max_quantity = std::max(lot.max_quantity, std::abs(lot.quantity));
        lot.fees += fill.commission;
        return std::nullopt;
    }

    const double size = std::abs(lot.quantity);
    const double closing = std::min(size, fill.quantity);
    const double direction = lot.quantity > 0.0 ? 1.0 : -1.0;
    lot.gross_pnl += (fill.price - lot.entry_price) * closing * direction;
    lot.exit_value += closing * fill.price;
    lot.exit_quantity += closing;
    lot.fees += fill.commission * closing / fill.quantity;
    lot.quantity -= closing * direction;

    if (std::abs(lot.quantity) > quantityEpsilon(lot.max_quantity))
        return std::nullopt;

    lot.quantity = 0.0;
    lot.closed_at = fill.timestamp;
    lot.closing_fill_id = fill.fill_id;
    PositionLot closed = lot;
    closed_.push_back(closed);
    open_.erase(found);

    const double remainder = fill.quantity - closing;
    if (remainder > quantityEpsilon(fill.quantity))
        openLot(fill, remainder, fill.commission * remainder / fill.quantity);
    return closed;
}


void PositionLedger::applyFunding(const FundingPayment& payment)
{
    if (!std::isfinite(payment.amount))
        throw std::invalid_argument("Funding amount must be finite");
    if (!payment.payment_id.empty() && !applied_funding_.insert(payment.payment_id).second)
        return;
    const auto found = open_.find(payment.coin);
    if (found != open_.end()) {
        found->second.funding += payment.amount;
        return;
    }
    // Funding dated on the day a position closed belongs to that position: cash already
    // holds it, so dropping it would break the PnL breakdown.
    const auto closed = std::find_if(closed_.rbegin(), closed_.rend(),
        [&](const PositionLot& lot) { return lot.coin == payment.coin; });
    if (closed != closed_.rend())
        closed->funding += payment.amount;
}


PositionLedger PositionLedger::rebuild(std::vector<Fill> fills, std::vector<FundingPayment> funding)
{
    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
        return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.fill_id < b.fill_id;
    });
    std::stable_sort(funding.begin(), funding.end(), [](const FundingPayment& a, const FundingPayment& b) {
        return a.timestamp < b.timestamp;
    });

    // Funding at a fill's timestamp belongs to the position held up to it, so it goes first.
    // Live timestamps are whole days (fills at the 00:00 open, funding hourly after them):
    // same-day funding for an asset with no position yet belongs to the one the day's fills
    // open, so it waits until those fills are applied (found 2026-10-23: XRP's opening-day
    // funding was missing from its closed-position row).
    PositionLedger ledger;
    std::size_t next = 0;
    std::vector<FundingPayment> afterFills;
    for (std::size_t index = 0; index < fills.size(); ++index) {
        const Fill& fill = fills[index];
        while (next < funding.size() && funding[next].timestamp < fill.timestamp)
            ledger.applyFunding(funding[next++]);
        while (next < funding.size() && funding[next].timestamp == fill.timestamp) {
            if (ledger.open_.contains(funding[next].coin))
                ledger.applyFunding(funding[next]);
            else
                afterFills.push_back(funding[next]);
            ++next;
        }
        ledger.applyFill(fill);
        if (index + 1 == fills.size() || fills[index + 1].timestamp != fill.timestamp) {
            for (const FundingPayment& payment : afterFills)
                ledger.applyFunding(payment);
            afterFills.clear();
        }
    }
    while (next < funding.size())
        ledger.applyFunding(funding[next++]);
    return ledger;
}


double PositionLedger::realizedGrossPnl() const
{
    double total = 0.0;
    for (const PositionLot& lot : closed_)
        total += lot.gross_pnl;
    for (const auto& entry : open_)
        total += entry.second.gross_pnl;
    return total;
}


double PositionLedger::fees() const
{
    double total = 0.0;
    for (const PositionLot& lot : closed_)
        total += lot.fees;
    for (const auto& entry : open_)
        total += entry.second.fees;
    return total;
}


double PositionLedger::funding() const
{
    double total = 0.0;
    for (const PositionLot& lot : closed_)
        total += lot.funding;
    for (const auto& entry : open_)
        total += entry.second.funding;
    return total;
}
