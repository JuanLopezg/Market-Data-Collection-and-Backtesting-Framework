#pragma once

#include <cstdint>
#include <string>

#include "execution_order.h"


// One deterministic identity vocabulary for the LIVE daily execution chain
//
// These identities describe logical/economic messages. Transport redelivery or process
// restart must reproduce the same ids for the same completed UTC day/state revision.
namespace LiveExecutionIdentity {

inline std::string notionalPlanningRequest(Timestamp decisionTimestamp, std::uint64_t stateRevision)
{
    return "notional-order-plan-request:" + std::to_string(decisionTimestamp) + ":" +
        std::to_string(stateRevision);
}

inline std::string notionalOrderPlan(Timestamp decisionTimestamp, std::uint64_t stateRevision)
{
    return "notional-order-plan:" + std::to_string(decisionTimestamp) + ":" +
        std::to_string(stateRevision);
}

inline std::string plannedEconomicOrder(
    Timestamp decisionTimestamp,
    std::uint64_t stateRevision,
    OrderID orderId)
{
    return "planned-notional-order:" + std::to_string(decisionTimestamp) + ":" +
        std::to_string(stateRevision) + ":" + std::to_string(orderId);
}

} // namespace LiveExecutionIdentity
