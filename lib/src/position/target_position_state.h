#pragma once

#include "position_state.h"


/**************************************************************************************
 * Type    : TargetPositionState
 * Purpose : Desired quantity by asset before execution
 *
 * It can represent one strategy target or the final net account target depending on
 * where it is used.
 **************************************************************************************/
using TargetPositionState = PositionState;
