#pragma once

#include "exchange_snapshot.h"
#include "reconciler.h"
#include "state_store.h"
#include "trading_engine.h"


/**************************************************************************************
 * Type    : RecoveryResult
 * Purpose : Startup recovery outcome exposed to the future live runtime
 **************************************************************************************/
struct RecoveryResult {
    bool restored_persisted_state = false;
    bool ready_for_trading = false;
    ReconciliationReport reconciliation;
};


/**************************************************************************************
 * Type    : RecoveryCoordinator
 * Purpose : Restore persisted state, rebuild analytics and reconcile before trading
 *
 * Trading remains paused on any mismatch. A clean reconciliation resumes the engine and
 * checkpoints the reconciled state. No automatic position/order repair is attempted.
 **************************************************************************************/
class RecoveryCoordinator {
private:
    Reconciler reconciler_;

public:
    explicit RecoveryCoordinator(Reconciler reconciler = Reconciler());

    RecoveryResult recover(
        TradingEngine& engine,
        StateStore& stateStore,
        const ExchangeSnapshot& exchangeSnapshot
    ) const;
};
