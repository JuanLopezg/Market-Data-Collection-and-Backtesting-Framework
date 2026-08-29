#include "recovery_coordinator.h"

#include <stdexcept>
#include <utility>


RecoveryCoordinator::RecoveryCoordinator(Reconciler reconciler)
    : reconciler_(std::move(reconciler))
{}


RecoveryResult RecoveryCoordinator::recover(
    TradingEngine& engine,
    StateStore& stateStore,
    const ExchangeSnapshot& exchangeSnapshot
) const
{
    RecoveryResult result;

    // Nothing may generate new strategy/execution work until local and exchange state agree.
    engine.pauseTrading();

    const auto persisted = stateStore.load();
    const std::vector<Fill> fills = stateStore.loadFills();

    if (!persisted && !fills.empty())
        throw std::runtime_error("Fill audit exists without an operational trading snapshot");

    if (persisted) {
        engine.restoreState(*persisted);
        engine.rebuildTradeRecorder(fills);
        result.restored_persisted_state = true;
    }

    result.reconciliation = reconciler_.compare(engine.stateSnapshot(), exchangeSnapshot);
    result.ready_for_trading = result.reconciliation.clean();

    if (result.ready_for_trading) {
        // Only a successfully reconciled startup may attach/overwrite operational state.
        engine.attachStateStore(stateStore);
        engine.resumeTrading();
        engine.checkpoint();
    }

    return result;
}
