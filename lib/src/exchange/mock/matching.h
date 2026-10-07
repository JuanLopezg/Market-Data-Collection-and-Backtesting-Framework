#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "venue_events.h"
#include "decimal.h"
#include "orders.h"
#include "catalog.h"
#include "rules.h"

// Deterministic synthetic OHLCV matching and fill generation for MOCK.
//
// This model is intentionally synthetic. It must never be described as historical L2,
// queue-position or venue-fill truth. Economic timestamps come only from the supplied
// market observation event timestamp; this code never reads wall/monotonic clocks.
namespace MockVenue {

using VenueContracts::V1::Error;
using VenueContracts::V1::ErrorClass;
using VenueContracts::V1::Event;
using VenueContracts::V1::Fill;
using VenueContracts::V1::OrderLifecycleStatus;
using VenueContracts::V1::Side;
using VenueContracts::V1::TimeInForce;

struct MatchingFillConfig {
    std::uint32_t participation_ppm = 100000U; // 10%
    std::uint32_t max_adverse_slippage_ppm = 250U; // 2.5 bps
    std::uint32_t fee_ppm = 400U; // 4 bps used for matching diagnostics; accounting applies fees separately.
    bool ignore_volume_capacity = false;
    std::uint32_t latency_events = 0U;
    std::uint64_t deterministic_seed = 510051ULL;

    bool valid() const
    {
        return participation_ppm <= 1000000U &&
               max_adverse_slippage_ppm <= 1000000U &&
               fee_ppm <= 1000000U;
    }
};

struct MarketBarObservation {
    std::string canonical_asset;
    Timestamp event_time = 0;
    OHLCV bar;

    bool valid() const
    {
        if (canonical_asset.empty() ||
            !std::isfinite(bar.open) || bar.open <= 0.0 ||
            !std::isfinite(bar.high) || bar.high <= 0.0 ||
            !std::isfinite(bar.low) || bar.low <= 0.0 ||
            !std::isfinite(bar.close) || bar.close <= 0.0 ||
            !std::isfinite(bar.volume) || bar.volume < 0.0)
            return false;

        return bar.high >= bar.open &&
               bar.high >= bar.close &&
               bar.high >= bar.low &&
               bar.low <= bar.open &&
               bar.low <= bar.close;
    }
};

enum class MatchAction {
    DeferredLatency = 0,
    Rested,
    NoChange,
    RejectedPostOnly,
    FilledPartial,
    FilledComplete,
    IocCanceledNoFill,
    IocCanceledRemainder
};

struct MatchTrace {
    OrderID local_order_id = 0;
    Timestamp event_time = 0;
    MatchAction action = MatchAction::NoChange;
    double requested_remaining_quantity = 0.0;
    double fill_quantity = 0.0;
    double fill_price = 0.0;
    std::uint32_t slippage_ppm_applied = 0U;
    double diagnostic_fee_quote = 0.0;
    std::uint64_t synthetic_liquidity_units_before = 0U;
    std::uint64_t synthetic_liquidity_units_after = 0U;
    std::string native_fill_id;
};

class MockMatching {
public:
    using EventHandler = std::function<void(const Event&)>;

    MockMatching(
        MockOrders& lifecycle,
        MatchingFillConfig config = {});

    void setEventHandler(EventHandler handler);

    const MatchingFillConfig& config() const;
    const std::vector<Event>& emittedEvents() const;
    const std::vector<MatchTrace>& traces() const;

    void clearOutput();
    void processBar(const MarketBarObservation& observation);

private:
    MockOrders& lifecycle_;
    MatchingFillConfig config_;
    EventHandler handler_;
    std::vector<Event> emitted_events_;
    std::vector<MatchTrace> traces_;
    std::map<OrderID, std::uint32_t> latency_events_seen_;
    std::map<OrderID, std::uint32_t> fill_sequence_by_order_;

    static Error noError();

    static Error makeError(
        ErrorClass classification,
        const std::string& detail,
        const std::string& native_reason);

    static bool doubleToScaledUnitsFloor(
        double value,
        std::size_t scale,
        std::uint64_t* out);

    static std::uint64_t mulPpmFloor(
        std::uint64_t value,
        std::uint32_t ppm);

    static double scaledUnitsToDouble(
        std::uint64_t units,
        std::size_t scale);

    bool latencySatisfied(
        const StoredOrder& stored,
        const MarketBarObservation& observation);

    static bool crossesSyntheticArrival(
        const StoredOrder& stored,
        const MarketBarObservation& observation);

    static bool limitTouched(
        const StoredOrder& stored,
        const MarketBarObservation& observation);

    void handleNoExecution(
        const StoredOrder& stored,
        const MarketBarObservation& observation,
        std::uint64_t liquidity_units);

    std::uint32_t deterministicSlippagePpm(
        OrderID order_id,
        Timestamp event_time,
        std::uint32_t fill_sequence) const;

    static double syntheticFillPrice(
        const StoredOrder& stored,
        const MarketBarObservation& observation,
        const RuleProfile& rules,
        std::uint32_t slippage_ppm);

    void forwardLifecycleTransition(
        OrderID order_id,
        Timestamp timestamp,
        OrderLifecycleStatus next_status,
        double cumulative,
        double remaining,
        const std::string& native_status,
        const Error& reason);

    // Kept in the header because this helper is templated.
    template <typename T>
    void emit(const T& value)
    {
        Event event = value;
        emitted_events_.push_back(event);
        if (handler_)
            handler_(event);
    }
};

} // namespace MockVenue
