#pragma once

#include <cstdint>
#include <string>

#include "clock_state.h"
#include "contract_metadata.h"


/**************************************************************************************
 * Type    : ClockControlAction
 * Purpose : REPLAY-only operator commands for the shared logical clock authority.
 **************************************************************************************/
enum class ClockControlAction : int {
    Pause = 1,
    Resume = 2,
    SetSpeed = 3
};


/**************************************************************************************
 * Type    : ClockControl
 * Purpose : Durable REPLAY-only control-plane command consumed by replay-controller.
 *
 * expected_revision implements optional optimistic concurrency.  Zero means "apply to
 * the currently active revision".  Dashboard/backend callers should normally send the
 * revision they are displaying so a stale UI cannot silently change a newer replay.
 *
 * For SetSpeed:
 *   Realtime   -> speed_multiplier must be 0 (x1 wall-time semantics)
 *   Multiplier -> speed_multiplier must be finite and > 0
 *   MaxSpeed   -> speed_multiplier must be 0 (event-driven / no pacing wait)
 *
 * Pause/Resume preserve the current mode/speed and ignore the supplied speed fields.
 **************************************************************************************/
struct ClockControl {
    ContractMetadata metadata;
    std::string simulation_id;
    ClockControlAction action = ClockControlAction::Pause;
    SimulationClockMode mode = SimulationClockMode::MaxSpeed;
    double speed_multiplier = 0.0;
    std::uint64_t expected_revision = 0;
};
