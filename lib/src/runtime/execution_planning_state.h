#pragma once

#include <cstdint>
#include <vector>

#include "order_manager.h"
#include "order_planning.h"
#include "strategy_position_snapshot.h"


/**************************************************************************************
 * Build the immutable planner input owned by execution-state.
 *
 * state_revision is a deterministic FNV-1a fingerprint over exactly the execution
 * state that can affect planning. It survives process restart because it is derived
 * from persisted state rather than from an in-memory counter.
 **************************************************************************************/
ExecutionPlanningStateSnapshot makeExecutionPlanningStateSnapshot(
    const std::vector<StrategyID>& strategyIds,
    const StrategyPositionSnapshot& strategyPositions,
    const OrderManager& orderManager,
    OrderID nextOrderId
);

std::uint64_t executionPlanningStateRevision(
    const ExecutionPlanningStateSnapshot& state
);
