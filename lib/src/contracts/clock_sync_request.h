#pragma once

#include <cstdint>
#include <string>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : ClockSyncRequest
 * Purpose : REPLAY-only request asking the clock authority to re-publish its latest
 *           durable ClockState so a restarted follower can re-synchronize immediately.
 *
 * simulation_id may be empty when the follower has not learned the active simulation
 * identity yet. Once a follower has synchronized, it sends the learned identity on any
 * later request and rejects clock states from a different simulation.
 **************************************************************************************/
struct ClockSyncRequest {
    ContractMetadata metadata;
    std::string requester_id;
    std::string simulation_id;
    std::uint64_t known_revision = 0;
};
