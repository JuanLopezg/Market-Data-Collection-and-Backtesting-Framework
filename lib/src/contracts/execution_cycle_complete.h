#pragma once

#include <cstdint>

#include "contract_metadata.h"


/**************************************************************************************
 * Type    : ExecutionCycleComplete
 * Purpose : Authoritative execution barrier emitted after one decision/open cycle
 *
 * The event means:
 * - the order plan for decision T / execution T+1 was accepted and persisted;
 * - every Fill observed for that cycle was persisted before this event;
 * - there are no locally-open orders remaining when the event is emitted.
 *
 * It is useful to accelerated replay, but it remains an execution-domain event rather
 * than a replay-specific command: the execution-state service is the only authority
 * that can safely declare its own cycle quiescent.
 **************************************************************************************/
struct ExecutionCycleComplete {
    ContractMetadata metadata;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::uint64_t state_revision = 0;
};
