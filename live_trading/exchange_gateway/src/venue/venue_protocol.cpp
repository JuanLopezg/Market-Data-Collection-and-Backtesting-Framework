#include "venue_protocol.h"

namespace Venue {

const char* endpointName(Endpoint endpoint)
{
    switch (endpoint) {
        case Endpoint::Permissions:    return "permissions";
        case Endpoint::Instruments:    return "instruments";
        case Endpoint::Tickers:        return "tickers";
        case Endpoint::Accounts:       return "accounts";
        case Endpoint::Positions:      return "positions";
        case Endpoint::OpenOrders:     return "open orders";
        case Endpoint::Fills:          return "fills";
        case Endpoint::SendOrder:      return "send order";
        case Endpoint::CancelOrder:    return "cancel order";
        case Endpoint::DeadMansSwitch: return "dead man's switch";
        case Endpoint::AccountLog:     return "account log";
    }
    return "unknown";
}

} // namespace Venue
