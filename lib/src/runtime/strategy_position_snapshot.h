#pragma once

#include <unordered_map>

#include "data_types.h"
#include "virtual_position_state.h"


/**************************************************************************************
 * Type    : StrategyPositionSnapshot
 * Purpose : Execution-owned filled quantities exposed read-only to decision logic
 *
 * Decision may inspect this snapshot when applying rebalance policy, but only Execution
 * mutates the authoritative quantities in response to actual Fill events.
 **************************************************************************************/
using StrategyPositionSnapshot = std::unordered_map<StrategyID, VirtualPositionState>;
