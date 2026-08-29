#pragma once

#include "position_state.h"


/**************************************************************************************
 * Type    : VirtualPositionState
 * Purpose : Filled quantities attributed internally to one strategy
 *
 * This is strategy-level accounting state, not a separate exchange position. Several
 * strategies may therefore hold opposing virtual quantities while the real account only
 * owns their net quantity.
 *
 * It must be updated from fills/internal allocation, never merely because a strategy
 * requested a new target.
 **************************************************************************************/
using VirtualPositionState = PositionState;
