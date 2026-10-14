#pragma once

#include "exchange_snapshot.h"
#include "reconciliation_report.h"
#include "trading_state_snapshot.h"


// Compare local operational state with normalized exchange state
//
// V1 is intentionally conservative: any meaningful cash/position/open-order mismatch is
// blocking. Automatic repair belongs later, after exchange-specific behaviour is known.
//
// Quantities match when they differ by at most quantityTolerance units OR by a relative
// QUANTITY_RELATIVE_TOLERANCE of the larger one. A double carries ~16 significant digits,
// so above ~1 million units summed fills differ by more than 1e-10 from pure rounding;
// a fixed tolerance alone would report false drift (and pause trading) for cheap coins
// held in millions of units. A real mismatch is at least one venue size step, which is
// far above 1e-12 of any realistic position.
class Reconciler {
private:
    static constexpr double QUANTITY_RELATIVE_TOLERANCE = 1e-12;

    double cash_tolerance_ = 1e-6;
    double quantity_tolerance_ = 1e-10;

    bool sameQuantity(double local, double exchange) const;

public:
    Reconciler(double cashTolerance = 1e-6, double quantityTolerance = 1e-10);

    ReconciliationReport compare(
        const TradingStateSnapshot& local,
        const ExchangeSnapshot& exchange
    ) const;
};
