#pragma once

#include <cstdint>
#include <string>

#include "data_types.h"


/**************************************************************************************
 * Type    : ContractMetadata
 * Purpose : Transport-independent metadata shared by service-boundary DTOs
 *
 * message_id is producer-owned and is the idempotency key for a logical message.
 * correlation_id links commands/events belonging to the same workflow without imposing
 * any transport-specific identifier type.
 **************************************************************************************/
struct ContractMetadata {
    std::uint32_t schema_version = 1;
    std::string message_id;
    std::string correlation_id;
    Timestamp produced_at = 0;
};
