// Implements the daily equity rows of ExecutionState's accounting projection.

#include "equity_history.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "time_utils.h"

Timestamp tradingDayOf(Timestamp timestamp)
{
    return timestamp >= 100000000 ? timestamp / 10 : timestamp;
}


EquityDay markEquityDay(
    Timestamp day,
    double cash,
    const std::unordered_map<Coin, double>& positions,
    const std::unordered_map<Coin, double>& closes,
    const PositionLedger& ledger,
    const std::vector<VenueCostEvent>& accountMovements,
    double collateralUsd)
{
    EquityDay row;
    row.date = day;
    row.cash = cash;
    for (const auto& [coin, quantity] : positions)
        if (quantity != 0.0)
            row.positions_value += quantity * closes.at(coin);
    row.equity = row.cash + row.positions_value;
    row.realized_gross_pnl = ledger.realizedGrossPnl();
    row.fees = ledger.fees();
    row.funding = ledger.funding();
    row.open_positions = ledger.openPositions().size();
    for (const auto& [coin, lot] : ledger.openPositions())
        row.open_cost += lot.quantity * lot.entry_price;
    for (const VenueCostEvent& movement : accountMovements) {
        if (tradingDayOf(movement.timestamp) > day)
            break;
        (movement.kind == VenueCostKind::CapitalFlow ? row.capital_flows : row.interest) += movement.amount_usd;
    }
    row.collateral_usd = collateralUsd;
    return row;
}


bool fillsReachAccount(
    const std::vector<Fill>& fills,
    const std::vector<FundingPayment>& funding,
    const std::vector<VenueCostEvent>& accountMovements,
    double initialCash,
    const Account& live)
{
    Account replayed(initialCash);
    for (const Fill& fill : fills)
        replayed.applyFill(fill);
    for (const FundingPayment& payment : funding)
        replayed.applyCashMovement(payment.amount);
    for (const VenueCostEvent& movement : accountMovements)
        replayed.applyCashMovement(movement.amount_usd);

    const auto& livePositions = live.positions().values();
    bool matches = std::abs(replayed.cash() - live.cash()) <= 1e-6 * std::max(1.0, std::abs(replayed.cash()));
    const auto& replayedPositions = replayed.positions().values();
    for (const auto& [coin, quantity] : replayedPositions) {
        const auto found = livePositions.find(coin);
        const double current = found == livePositions.end() ? 0.0 : found->second;
        matches = matches && std::abs(current - quantity) <= 1e-9 * std::max(1.0, std::abs(quantity));
    }
    // A live position the fills never opened is not explained by them either.
    for (const auto& [coin, quantity] : livePositions)
        if (quantity != 0.0 && !replayedPositions.contains(coin))
            matches = false;
    return matches;
}


std::vector<EquityDay> equityDaysFromFills(
    const std::vector<Fill>& fills,
    const std::vector<FundingPayment>& funding,
    const std::vector<VenueCostEvent>& accountMovements,
    double initialCash,
    Timestamp lastDay,
    const std::set<Timestamp>& existing,
    const DayClosesLoader& closesFor,
    const std::function<double(Timestamp day)>& collateralFor)
{
    std::vector<EquityDay> rows;
    if (fills.empty())
        return rows;

    Account account(initialCash);
    PositionLedger ledger;
    std::size_t nextFill = 0;
    std::size_t nextFunding = 0;
    std::size_t nextMovement = 0;
    std::unordered_set<FillID> appliedFillIds;
    for (Timestamp day = tradingDayOf(fills.front().timestamp); day <= lastDay;
         day = static_cast<Timestamp>(nextDay(static_cast<unsigned int>(day)))) {
        // Everything dated up to this day, so the row is the account at close(day): a fill
        // of the next day's open belongs to the next row. The day's fills (at its open) go
        // first: funding dated on that day accrues on the positions they opened.
        while (nextFill < fills.size() && tradingDayOf(fills[nextFill].timestamp) <= day) {
            // The ledger ignores a repeated fill id; the account must too, or the row's
            // cash and positions no longer match its PnL breakdown.
            const Fill& fill = fills[nextFill++];
            if (fill.fill_id != 0 && !appliedFillIds.insert(fill.fill_id).second)
                continue;
            account.applyFill(fill);
            ledger.applyFill(fill);
        }
        while (nextFunding < funding.size() && tradingDayOf(funding[nextFunding].timestamp) <= day) {
            account.applyCashMovement(funding[nextFunding].amount);
            ledger.applyFunding(funding[nextFunding++]);
        }
        while (nextMovement < accountMovements.size() && tradingDayOf(accountMovements[nextMovement].timestamp) <= day)
            account.applyCashMovement(accountMovements[nextMovement++].amount_usd);
        if (existing.contains(day))
            continue;

        std::set<Coin> coins;
        for (const auto& [coin, quantity] : account.positions().values())
            if (quantity != 0.0)
                coins.insert(coin);
        if (const auto closes = closesFor(day, coins))
            rows.push_back(markEquityDay(day, account.cash(), account.positions().values(), *closes, ledger,
                                         accountMovements, collateralFor(day)));
    }
    return rows;
}
