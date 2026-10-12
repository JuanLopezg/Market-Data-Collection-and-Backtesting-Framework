#include "venue_protocol.h"

#include <array>
#include <stdexcept>

namespace Venue {

namespace {

// Binance contracts quoted per 1000 (or 1,000,000) underlying units.
// Keep in sync with live_trading/kraken_shadow.py source_asset().
struct ScaledAsset {
    const char* source_base;
    const char* asset;
    double unit_scale;
};

constexpr std::array<ScaledAsset, 9> SCALED_ASSETS{{
    {"1000PEPE", "PEPE", 1000.0},
    {"1000SHIB", "SHIB", 1000.0},
    {"1000BONK", "BONK", 1000.0},
    {"1000FLOKI", "FLOKI", 1000.0},
    {"1000LUNC", "LUNC", 1000.0},
    {"1000DOGS", "DOGS", 1000.0},
    {"1000SATS", "SATS", 1000.0},
    {"1000CHEEMS", "CHEEMS", 1000.0},
    {"1000000MOG", "MOG", 1000000.0},
}};

} // namespace


SourceAsset sourceAssetForCoin(const Coin& coin)
{
    const std::string suffix = "USDT";
    if (coin.size() <= suffix.size() || coin.compare(coin.size() - suffix.size(), suffix.size(), suffix) != 0)
        throw std::invalid_argument("Expected a Binance USDT source symbol, got " + coin);

    const std::string base = coin.substr(0, coin.size() - suffix.size());
    for (const ScaledAsset& scaled : SCALED_ASSETS) {
        if (base == scaled.source_base)
            return {scaled.asset, scaled.unit_scale};
    }
    return {base, 1.0};
}


Coin coinForSourceAsset(const std::string& asset)
{
    for (const ScaledAsset& scaled : SCALED_ASSETS) {
        if (asset == scaled.asset)
            return std::string(scaled.source_base) + "USDT";
    }
    return asset + "USDT";
}


const char* endpointName(Endpoint endpoint)
{
    switch (endpoint) {
        case Endpoint::Permissions:    return "permissions";
        case Endpoint::Instruments:    return "instruments";
        case Endpoint::Accounts:       return "accounts";
        case Endpoint::Positions:      return "positions";
        case Endpoint::OpenOrders:     return "open orders";
        case Endpoint::Fills:          return "fills";
        case Endpoint::SendOrder:      return "send order";
        case Endpoint::CancelOrder:    return "cancel order";
        case Endpoint::DeadMansSwitch: return "dead man's switch";
    }
    return "unknown";
}

} // namespace Venue
