#pragma once

#include "exchange_snapshot.h"
#include "reconciliation_report.h"
#include "trading_state_snapshot.h"


/**************************************************************************************
 * Type    : Reconciler
 * Purpose : Compare local operational state with normalized exchange state
 *
 * V1 is intentionally conservative: any meaningful cash/position/open-order mismatch is
 * blocking. Automatic repair belongs later, after exchange-specific behaviour is known.
 **************************************************************************************/
class Reconciler {
private:
    double cash_tolerance_ = 1e-6;
    double quantity_tolerance_ = 1e-10;

public:
    Reconciler(double cashTolerance = 1e-6, double quantityTolerance = 1e-10);

    ReconciliationReport compare(
        const TradingStateSnapshot& local,
        const ExchangeSnapshot& exchange
    ) const;
};
