#pragma once


/**************************************************************************************
 * Type    : ExecutionOrderStatus
 * Purpose : Generic lifecycle state tracked for a new execution-layer order
 *
 * The explicit name avoids colliding with the legacy backtest OrderStatus still kept in
 * data_types.h for old strategies/reporting compatibility.
 **************************************************************************************/
enum class ExecutionOrderStatus {
    Created,
    Submitted,
    Accepted,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected
};


inline bool isTerminalExecutionOrderStatus(ExecutionOrderStatus status)
{
    return status == ExecutionOrderStatus::Filled ||
           status == ExecutionOrderStatus::Canceled ||
           status == ExecutionOrderStatus::Rejected;
}
