#pragma once

#include <optional>
#include <vector>

#include "fill.h"
#include "trading_state_snapshot.h"


/**************************************************************************************
 * Type    : StateStore
 * Purpose : Persistence boundary for live/restart state
 *
 * The domain/runtime does not know whether state is stored in SQLite, another process or
 * a remote service. save() must be atomic: when newFill is supplied, the fill audit row
 * and operational snapshot must either both commit or both roll back.
 **************************************************************************************/
class StateStore {
public:
    virtual ~StateStore() = default;

    virtual void save(
        const TradingStateSnapshot& snapshot,
        const std::optional<Fill>& newFill = std::nullopt
    ) = 0;

    virtual std::optional<TradingStateSnapshot> load() const = 0;
    virtual std::vector<Fill> loadFills() const = 0;
};
