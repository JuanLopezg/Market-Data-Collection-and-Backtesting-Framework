#pragma once

#include "contract_metadata.h"
#include "execution_order.h"


/**************************************************************************************
 * Commands crossing the ExecutionEngine -> Exchange boundary.
 **************************************************************************************/
struct SubmitOrderCommand {
    ContractMetadata metadata;
    ExecutionOrder order;
};

struct CancelOrderCommand {
    ContractMetadata metadata;
    OrderID order_id = 0;
    Timestamp requested_at = 0;
};
