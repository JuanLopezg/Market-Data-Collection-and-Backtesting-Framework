#pragma once

#include <functional>

#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "execution_commands.h"
#include "execution_events.h"


/**************************************************************************************
 * Type    : ExchangeGatewayHandlers
 * Purpose : Normalized asynchronous events produced by one concrete exchange adapter
 **************************************************************************************/
struct ExchangeGatewayHandlers {
    std::function<void(const OrderUpdateEvent&)> on_order_update;
    std::function<void(const FillEvent&)> on_fill;
    std::function<void(const ExchangeSnapshotEvent&)> on_snapshot;
};


/**************************************************************************************
 * Type    : ExchangeGatewayAdapter
 * Purpose : Exchange-specific backend hidden behind the common gateway process
 *
 * A concrete live adapter owns its venue-native protocol/authentication details. A replay
 * adapter may route to a simulated/mock venue. Everything north of this boundary remains
 * transport- and venue-neutral; Step 48 will replace/freeze the canonical multi-venue
 * adapter contract after the Step 47 architecture/semantics gates are complete.
 **************************************************************************************/
class ExchangeGatewayAdapter {
public:
    virtual ~ExchangeGatewayAdapter() = default;

    virtual void setHandlers(ExchangeGatewayHandlers handlers) = 0;
    virtual void submitOrder(const SubmitOrderCommand& command) = 0;
    virtual void cancelOrder(const CancelOrderCommand& command) = 0;
    virtual void requestSnapshot(const ExchangeSnapshotRequest& request) = 0;
    virtual void poll(int timeoutMs) = 0;
};
