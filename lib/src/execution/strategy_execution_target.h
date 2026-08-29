#pragma once

#include "data_types.h"
#include "target_position_state.h"


/**************************************************************************************
 * Type    : StrategyExecutionTarget
 * Purpose : Quantity target for one strategy after sizing/rebalance resolution
 **************************************************************************************/
struct StrategyExecutionTarget {
    StrategyID strategy_id = 0;
    TargetPositionState positions;
};
