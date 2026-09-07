#pragma once

#include <cstdint>
#include <string>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : SimulationClockMode
 * Purpose : Operating mode advertised by the REPLAY-only logical clock authority.
 *
 * STEP 35F activates all three modes. Realtime maps simulated seconds 1:1 to wall
 * seconds, Multiplier accelerates that mapping, and MaxSpeed remains event-driven.
 * The economic event/barrier graph is identical in every mode.
 **************************************************************************************/
enum class SimulationClockMode : int {
    Realtime = 1,
    Multiplier = 2,
    MaxSpeed = 3
};


/**************************************************************************************
 * Type    : ClockState
 * Purpose : Durable, monotonic logical-time state shared by distributed REPLAY services.
 *
 * logical_time intentionally uses the project's existing Timestamp type. The current
 * historical runtime is daily (YYYYMMDD); changing the economic timestamp model is a
 * separate migration and is not part of the shared-clock introduction.
 **************************************************************************************/
struct ClockState {
    ContractMetadata metadata;
    std::string simulation_id;
    Timestamp logical_time = 0;
    std::uint64_t revision = 0;
    SimulationClockMode mode = SimulationClockMode::MaxSpeed;
    double speed_multiplier = 0.0; // >0 only for Multiplier; 0 for MaxSpeed.
    bool paused = false;
};
