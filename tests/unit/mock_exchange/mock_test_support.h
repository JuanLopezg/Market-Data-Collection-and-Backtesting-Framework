#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "mock/exchange.h"
#include "test_support.h"

namespace mock_test {
namespace M = MockVenue;
namespace V = VenueContracts::V1;

inline V::RequestIdentity request(const std::string& id, Timestamp at = 10)
{
    return {M::context(), id, "correlation-" + id, at};
}

inline V::LimitOrderIntent order(OrderID id = 1, V::Side side = V::Side::Buy,
                                double quantity = 0.2, double limit = 100.0,
                                const std::string& asset = "BTCUSDT")
{
    const auto* entry = M::findByCanonicalAssetExact(asset);
    if (!entry) throw std::invalid_argument("Unknown synthetic fixture asset");
    V::LimitOrderIntent result;
    result.local_order_id = id;
    result.strategy_id = 7;
    result.created_at = 10;
    result.active_from = 20;
    result.instrument = entry->instrument();
    result.side = side;
    result.quantity = quantity;
    result.limit_price = limit;
    result.client_order_id = "client-" + std::to_string(id);
    return result;
}

inline V::SubmitOrderBatch submit(const V::LimitOrderIntent& intent = order(),
                                  const std::string& id = "submit-1", Timestamp at = 10)
{
    return {request(id, at), {{"item-" + id, intent}}};
}

inline V::CancelOrderBatch cancel(const V::LimitOrderIntent& intent = order(),
                                  const std::string& id = "cancel-1", Timestamp at = 30)
{
    V::OrderLocator locator;
    locator.local_order_id = intent.local_order_id;
    locator.instrument = intent.instrument;
    return {request(id, at), {{"item-" + id, locator}}};
}

inline V::ModifyOrderBatch modify(const V::LimitOrderIntent& original,
                                  const V::LimitOrderIntent& replacement,
                                  const std::string& id = "modify-1", Timestamp at = 25)
{
    V::OrderLocator locator;
    locator.local_order_id = original.local_order_id;
    locator.instrument = original.instrument;
    return {request(id, at), {{"item-" + id, locator, replacement}}};
}

inline M::MatchingFillConfig matching()
{
    M::MatchingFillConfig config;
    config.max_adverse_slippage_ppm = 0;
    return config;
}

inline M::MarketBarObservation bar(Timestamp at = 20, double volume = 1.0,
                                   double open = 100.0, double high = 101.0,
                                   double low = 99.0, double close = 100.0,
                                   const std::string& asset = "BTCUSDT")
{
    M::MarketBarObservation result;
    result.canonical_asset = asset;
    result.event_time = at;
    result.bar = {open, high, low, close, volume};
    return result;
}

inline V::Fill fill(const std::string& id = "fill-1", V::Side side = V::Side::Buy,
                    double quantity = 0.2, double price = 100.0, Timestamp at = 20)
{
    const auto intent = order(1, side, quantity, price);
    V::Fill result;
    result.venue = M::context();
    result.instrument = intent.instrument;
    result.local_order_id = intent.local_order_id;
    result.strategy_id = intent.strategy_id;
    result.timestamp = at;
    result.side = side;
    result.quantity = quantity;
    result.price = price;
    result.native_references.native_order_id = "mock-order-1";
    result.native_references.native_client_order_id = intent.client_order_id;
    result.native_references.native_fill_id = id;
    return result;
}

template <class T>
std::vector<T> eventsOf(const std::vector<V::Event>& events)
{
    std::vector<T> selected;
    for (const auto& event : events)
        if (const auto* value = std::get_if<T>(&event)) selected.push_back(*value);
    return selected;
}

inline bool hasIssue(const M::ReconciliationReport& report, M::ReconciliationIssueKind kind)
{
    return std::any_of(report.issues.begin(), report.issues.end(), [kind](const auto& issue) {
        return issue.kind == kind;
    });
}

} // namespace mock_test
