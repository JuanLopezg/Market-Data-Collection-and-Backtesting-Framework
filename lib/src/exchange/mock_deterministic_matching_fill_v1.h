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

#include "canonical_venue_events_v1.h"
#include "mock_decimal_grid_v1.h"
#include "mock_order_admission_lifecycle_v1.h"
#include "mock_venue_catalog_v1.h"
#include "mock_venue_rules_v1.h"

/**************************************************************************************
 * Purpose : Deterministic synthetic OHLCV matching and fill generation for MOCK.
 *
 * This model is intentionally synthetic. It must never be described as historical L2,
 * queue-position or venue-fill truth. Economic timestamps come only from the supplied
 * market observation event timestamp; this code never reads wall/monotonic clocks.
 **************************************************************************************/
namespace MockVenueV1 {

using VenueContracts::V1::Error;
using VenueContracts::V1::ErrorClass;
using VenueContracts::V1::Event;
using VenueContracts::V1::Fill;
using VenueContracts::V1::OrderLifecycleStatus;
using VenueContracts::V1::Side;
using VenueContracts::V1::TimeInForce;

struct MatchingFillConfigV1 {
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

struct MarketBarObservationV1 {
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

enum class MatchActionV1 {
    DeferredLatency = 0,
    Rested,
    NoChange,
    RejectedPostOnly,
    FilledPartial,
    FilledComplete,
    IocCanceledNoFill,
    IocCanceledRemainder
};

struct MatchTraceV1 {
    OrderID local_order_id = 0;
    Timestamp event_time = 0;
    MatchActionV1 action = MatchActionV1::NoChange;
    double requested_remaining_quantity = 0.0;
    double fill_quantity = 0.0;
    double fill_price = 0.0;
    std::uint32_t slippage_ppm_applied = 0U;
    double diagnostic_fee_quote = 0.0;
    std::uint64_t synthetic_liquidity_units_before = 0U;
    std::uint64_t synthetic_liquidity_units_after = 0U;
    std::string native_fill_id;
};

class MockDeterministicMatchingFillV1 {
public:
    using EventHandler = std::function<void(const Event&)>;

    MockDeterministicMatchingFillV1(
        MockOrderAdmissionLifecycleV1& lifecycle,
        MatchingFillConfigV1 config = {});

    void setEventHandler(EventHandler handler);

    const MatchingFillConfigV1& config() const;
    const std::vector<Event>& emittedEvents() const;
    const std::vector<MatchTraceV1>& traces() const;

    void clearOutput();
    void processBar(const MarketBarObservationV1& observation);

private:
    MockOrderAdmissionLifecycleV1& lifecycle_;
    MatchingFillConfigV1 config_;
    EventHandler handler_;
    std::vector<Event> emitted_events_;
    std::vector<MatchTraceV1> traces_;
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
        const MarketBarObservationV1& observation);

    static bool crossesSyntheticArrival(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation);

    static bool limitTouched(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation);

    void handleNoExecution(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation,
        std::uint64_t liquidity_units);

    std::uint32_t deterministicSlippagePpm(
        OrderID order_id,
        Timestamp event_time,
        std::uint32_t fill_sequence) const;

    static double syntheticFillPrice(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation,
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

} // namespace MockVenueV1
