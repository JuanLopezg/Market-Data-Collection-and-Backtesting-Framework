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
 * Header  : mock_deterministic_matching_fill_v1.h
 * Step    : 51 — Deterministic Matching / Fill Model
 * Purpose : Explicit synthetic OHLCV matching model for MOCK
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
    std::uint32_t fee_ppm = 400U; // 4 bps, diagnostic only until Step52
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
        MatchingFillConfigV1 config = {})
        : lifecycle_(lifecycle), config_(config)
    {
        if (!config_.valid())
            throw std::invalid_argument("invalid MOCK Step51 matching/fill config");
    }

    void setEventHandler(EventHandler handler)
    {
        handler_ = std::move(handler);
    }

    const MatchingFillConfigV1& config() const { return config_; }
    const std::vector<Event>& emittedEvents() const { return emitted_events_; }
    const std::vector<MatchTraceV1>& traces() const { return traces_; }

    void clearOutput()
    {
        emitted_events_.clear();
        traces_.clear();
    }

    void processBar(const MarketBarObservationV1& observation)
    {
        if (!observation.valid())
            throw std::invalid_argument("invalid MOCK market bar observation");

        const CatalogEntry* catalog_entry =
            findByCanonicalAssetExact(observation.canonical_asset);
        if (catalog_entry == nullptr || !catalog_entry->enabled)
            return;

        const RuleProfile* rules = rulesForExact(*catalog_entry);
        if (rules == nullptr || !rules->valid())
            throw std::runtime_error("MOCK rule profile unavailable during matching");

        std::uint64_t liquidity_units = 0U;
        if (!config_.ignore_volume_capacity) {
            std::uint64_t bar_volume_units = 0U;
            if (!doubleToScaledUnitsFloor(
                    observation.bar.volume,
                    rules->size_scale,
                    &bar_volume_units))
                throw std::runtime_error(
                    "MOCK bar volume cannot be represented on size scale");

            liquidity_units = mulPpmFloor(
                bar_volume_units, config_.participation_ppm);
        }

        std::uint64_t size_increment_units = 0U;
        if (!parseUnsignedDecimalToScale(
                rules->size_increment,
                rules->size_scale,
                &size_increment_units) ||
            size_increment_units == 0U)
            throw std::runtime_error("invalid MOCK size increment");

        if (!config_.ignore_volume_capacity) {
            liquidity_units =
                (liquidity_units / size_increment_units) * size_increment_units;
        }

        std::vector<OrderID> candidate_ids;
        for (const auto& pair : lifecycle_.orders()) {
            const StoredOrder& stored = pair.second;
            if (!stored.active())
                continue;
            if (stored.intent.instrument.market.canonical_asset != observation.canonical_asset)
                continue;
            if (observation.event_time < stored.intent.active_from)
                continue;
            candidate_ids.push_back(pair.first);
        }
        std::sort(candidate_ids.begin(), candidate_ids.end());

        for (const OrderID order_id : candidate_ids) {
            const StoredOrder* current = lifecycle_.findOrder(order_id);
            if (current == nullptr || !current->active())
                continue;

            if (!latencySatisfied(*current, observation)) {
                MatchTraceV1 trace;
                trace.local_order_id = order_id;
                trace.event_time = observation.event_time;
                trace.action = MatchActionV1::DeferredLatency;
                trace.requested_remaining_quantity = current->remaining_quantity;
                trace.synthetic_liquidity_units_before = liquidity_units;
                trace.synthetic_liquidity_units_after = liquidity_units;
                traces_.push_back(trace);
                continue;
            }

            current = lifecycle_.findOrder(order_id);
            if (current == nullptr || !current->active())
                continue;

            if (current->intent.post_only && crossesSyntheticArrival(*current, observation)) {
                Error reason = makeError(
                    ErrorClass::PostOnlyWouldCross,
                    "post-only order crosses synthetic bar-open arrival reference",
                    "POST_ONLY_WOULD_CROSS_SYNTHETIC_OPEN");

                forwardLifecycleTransition(
                    order_id,
                    observation.event_time,
                    OrderLifecycleStatus::Rejected,
                    current->cumulative_filled_quantity,
                    current->remaining_quantity,
                    "POST_ONLY_REJECTED_SYNTHETIC_OPEN",
                    reason);

                MatchTraceV1 trace;
                trace.local_order_id = order_id;
                trace.event_time = observation.event_time;
                trace.action = MatchActionV1::RejectedPostOnly;
                trace.requested_remaining_quantity = current->remaining_quantity;
                trace.synthetic_liquidity_units_before = liquidity_units;
                trace.synthetic_liquidity_units_after = liquidity_units;
                traces_.push_back(trace);
                continue;
            }

            const bool touched = limitTouched(*current, observation);
            if (!touched ||
                (!config_.ignore_volume_capacity && liquidity_units == 0U)) {
                handleNoExecution(*current, observation, liquidity_units);
                continue;
            }

            std::uint64_t remaining_units = 0U;
            if (!doubleToScaledUnitsExact(
                    current->remaining_quantity,
                    rules->size_scale,
                    &remaining_units))
                throw std::runtime_error("active MOCK order remaining quantity left size grid");

            std::uint64_t fill_units =
                config_.ignore_volume_capacity
                ? remaining_units
                : std::min(remaining_units, liquidity_units);
            fill_units = (fill_units / size_increment_units) * size_increment_units;

            if (fill_units == 0U) {
                handleNoExecution(*current, observation, liquidity_units);
                continue;
            }

            const std::uint64_t before_liquidity =
                config_.ignore_volume_capacity
                ? remaining_units
                : liquidity_units;
            if (!config_.ignore_volume_capacity)
                liquidity_units -= fill_units;

            const std::uint32_t fill_sequence = ++fill_sequence_by_order_[order_id];
            const std::uint32_t slippage_ppm = deterministicSlippagePpm(
                order_id, observation.event_time, fill_sequence);

            const double fill_quantity = scaledUnitsToDouble(
                fill_units, rules->size_scale);
            const double fill_price = syntheticFillPrice(
                *current, observation, *rules, slippage_ppm);

            Fill fill;
            fill.venue = context();
            fill.instrument = current->intent.instrument;
            fill.local_order_id = current->intent.local_order_id;
            fill.strategy_id = current->intent.strategy_id;
            fill.timestamp = observation.event_time;
            fill.side = current->intent.side;
            fill.quantity = fill_quantity;
            fill.price = fill_price;
            fill.native_references = current->native_references;
            fill.native_references.native_fill_id =
                "mock-fill-" + std::to_string(order_id) + "-" +
                std::to_string(fill_sequence);

            if (!fill.valid())
                throw std::runtime_error("generated MOCK canonical fill is invalid");

            emit(fill);

            const double cumulative =
                current->cumulative_filled_quantity + fill_quantity;
            const double remaining = std::max(
                0.0, current->intent.quantity - cumulative);
            const bool complete = fill_units >= remaining_units;

            MatchTraceV1 trace;
            trace.local_order_id = order_id;
            trace.event_time = observation.event_time;
            trace.requested_remaining_quantity = current->remaining_quantity;
            trace.fill_quantity = fill_quantity;
            trace.fill_price = fill_price;
            trace.slippage_ppm_applied = slippage_ppm;
            trace.diagnostic_fee_quote =
                std::abs(fill_quantity * fill_price) *
                static_cast<double>(config_.fee_ppm) / 1000000.0;
            trace.synthetic_liquidity_units_before = before_liquidity;
            trace.synthetic_liquidity_units_after = liquidity_units;
            trace.native_fill_id = fill.native_references.native_fill_id;

            if (complete) {
                forwardLifecycleTransition(
                    order_id,
                    observation.event_time,
                    OrderLifecycleStatus::Filled,
                    cumulative,
                    0.0,
                    "SYNTHETIC_FULL_FILL",
                    noError());
                trace.action = MatchActionV1::FilledComplete;
                traces_.push_back(trace);
                continue;
            }

            forwardLifecycleTransition(
                order_id,
                observation.event_time,
                OrderLifecycleStatus::PartiallyFilled,
                cumulative,
                remaining,
                "SYNTHETIC_PARTIAL_FILL",
                noError());
            trace.action = MatchActionV1::FilledPartial;
            traces_.push_back(trace);

            const StoredOrder* after_partial = lifecycle_.findOrder(order_id);
            if (after_partial != nullptr &&
                after_partial->intent.time_in_force == TimeInForce::Ioc) {
                forwardLifecycleTransition(
                    order_id,
                    observation.event_time,
                    OrderLifecycleStatus::Canceled,
                    after_partial->cumulative_filled_quantity,
                    after_partial->remaining_quantity,
                    "IOC_REMAINDER_CANCELED",
                    noError());

                MatchTraceV1 cancel_trace;
                cancel_trace.local_order_id = order_id;
                cancel_trace.event_time = observation.event_time;
                cancel_trace.action = MatchActionV1::IocCanceledRemainder;
                cancel_trace.requested_remaining_quantity = after_partial->remaining_quantity;
                cancel_trace.synthetic_liquidity_units_before = liquidity_units;
                cancel_trace.synthetic_liquidity_units_after = liquidity_units;
                traces_.push_back(cancel_trace);
            }
        }
    }

private:
    MockOrderAdmissionLifecycleV1& lifecycle_;
    MatchingFillConfigV1 config_;
    EventHandler handler_;
    std::vector<Event> emitted_events_;
    std::vector<MatchTraceV1> traces_;
    std::map<OrderID, std::uint32_t> latency_events_seen_;
    std::map<OrderID, std::uint32_t> fill_sequence_by_order_;

    static Error noError()
    {
        Error e;
        e.classification = ErrorClass::None;
        e.retryable = false;
        return e;
    }

    static Error makeError(
        ErrorClass classification,
        const std::string& detail,
        const std::string& native_reason)
    {
        Error e;
        e.classification = classification;
        e.retryable = false;
        e.native_code = "MOCK_V1_MATCH";
        e.native_reason = native_reason;
        e.detail = detail;
        return e;
    }

    static bool doubleToScaledUnitsFloor(
        double value,
        std::size_t scale,
        std::uint64_t* out)
    {
        if (out == nullptr || !std::isfinite(value) || value < 0.0)
            return false;

        long double factor = 1.0L;
        for (std::size_t i = 0; i < scale; ++i)
            factor *= 10.0L;

        const long double scaled = static_cast<long double>(value) * factor;
        if (!std::isfinite(static_cast<double>(scaled)) ||
            scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return false;

        *out = static_cast<std::uint64_t>(std::floor(scaled + 1e-12L));
        return true;
    }

    static std::uint64_t mulPpmFloor(
        std::uint64_t value,
        std::uint32_t ppm)
    {
        constexpr std::uint64_t denominator = 1000000ULL;
        const std::uint64_t q = value / denominator;
        const std::uint64_t r = value % denominator;

        if (q > std::numeric_limits<std::uint64_t>::max() /
                static_cast<std::uint64_t>(ppm))
            throw std::overflow_error("MOCK participation calculation overflow");

        const std::uint64_t major = q * static_cast<std::uint64_t>(ppm);
        const std::uint64_t minor =
            (r * static_cast<std::uint64_t>(ppm)) / denominator;

        if (major > std::numeric_limits<std::uint64_t>::max() - minor)
            throw std::overflow_error("MOCK participation calculation overflow");
        return major + minor;
    }

    static double scaledUnitsToDouble(
        std::uint64_t units,
        std::size_t scale)
    {
        long double factor = 1.0L;
        for (std::size_t i = 0; i < scale; ++i)
            factor *= 10.0L;
        return static_cast<double>(static_cast<long double>(units) / factor);
    }

    bool latencySatisfied(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation)
    {
        auto& seen = latency_events_seen_[stored.intent.local_order_id];
        if (seen < config_.latency_events) {
            ++seen;
            return false;
        }
        (void)observation;
        return true;
    }

    static bool crossesSyntheticArrival(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation)
    {
        if (stored.intent.side == Side::Buy)
            return stored.intent.limit_price >= observation.bar.open;
        return stored.intent.limit_price <= observation.bar.open;
    }

    static bool limitTouched(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation)
    {
        if (stored.intent.side == Side::Buy)
            return observation.bar.low <= stored.intent.limit_price;
        return observation.bar.high >= stored.intent.limit_price;
    }

    void handleNoExecution(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation,
        std::uint64_t liquidity_units)
    {
        if (stored.intent.time_in_force == TimeInForce::Ioc) {
            forwardLifecycleTransition(
                stored.intent.local_order_id,
                observation.event_time,
                OrderLifecycleStatus::Canceled,
                stored.cumulative_filled_quantity,
                stored.remaining_quantity,
                "IOC_NO_EXECUTION_CANCELED",
                noError());

            MatchTraceV1 trace;
            trace.local_order_id = stored.intent.local_order_id;
            trace.event_time = observation.event_time;
            trace.action = MatchActionV1::IocCanceledNoFill;
            trace.requested_remaining_quantity = stored.remaining_quantity;
            trace.synthetic_liquidity_units_before = liquidity_units;
            trace.synthetic_liquidity_units_after = liquidity_units;
            traces_.push_back(trace);
            return;
        }

        if (stored.status == OrderLifecycleStatus::Accepted) {
            forwardLifecycleTransition(
                stored.intent.local_order_id,
                observation.event_time,
                OrderLifecycleStatus::Resting,
                stored.cumulative_filled_quantity,
                stored.remaining_quantity,
                "GTC_RESTING_NO_EXECUTION",
                noError());

            MatchTraceV1 trace;
            trace.local_order_id = stored.intent.local_order_id;
            trace.event_time = observation.event_time;
            trace.action = MatchActionV1::Rested;
            trace.requested_remaining_quantity = stored.remaining_quantity;
            trace.synthetic_liquidity_units_before = liquidity_units;
            trace.synthetic_liquidity_units_after = liquidity_units;
            traces_.push_back(trace);
        } else {
            MatchTraceV1 trace;
            trace.local_order_id = stored.intent.local_order_id;
            trace.event_time = observation.event_time;
            trace.action = MatchActionV1::NoChange;
            trace.requested_remaining_quantity = stored.remaining_quantity;
            trace.synthetic_liquidity_units_before = liquidity_units;
            trace.synthetic_liquidity_units_after = liquidity_units;
            traces_.push_back(trace);
        }
    }

    std::uint32_t deterministicSlippagePpm(
        OrderID order_id,
        Timestamp event_time,
        std::uint32_t fill_sequence) const
    {
        if (config_.max_adverse_slippage_ppm == 0U)
            return 0U;

        std::uint64_t hash = 1469598103934665603ULL ^ config_.deterministic_seed;
        auto mix = [&](std::uint64_t value) {
            for (unsigned i = 0; i < 8U; ++i) {
                hash ^= static_cast<unsigned char>((value >> (i * 8U)) & 0xffU);
                hash *= 1099511628211ULL;
            }
        };
        mix(static_cast<std::uint64_t>(order_id));
        mix(static_cast<std::uint64_t>(event_time));
        mix(static_cast<std::uint64_t>(fill_sequence));

        return static_cast<std::uint32_t>(
            hash % (static_cast<std::uint64_t>(config_.max_adverse_slippage_ppm) + 1ULL));
    }

    static double syntheticFillPrice(
        const StoredOrder& stored,
        const MarketBarObservationV1& observation,
        const RuleProfile& rules,
        std::uint32_t slippage_ppm)
    {
        const double limit = stored.intent.limit_price;
        const bool buy = stored.intent.side == Side::Buy;

        double reference = limit;
        if (buy && observation.bar.open <= limit)
            reference = observation.bar.open;
        else if (!buy && observation.bar.open >= limit)
            reference = observation.bar.open;

        const long double fraction =
            static_cast<long double>(slippage_ppm) / 1000000.0L;
        long double candidate = static_cast<long double>(reference);
        candidate = buy
            ? candidate * (1.0L + fraction)
            : candidate * (1.0L - fraction);

        if (buy)
            candidate = std::min(candidate, static_cast<long double>(limit));
        else
            candidate = std::max(candidate, static_cast<long double>(limit));

        std::uint64_t increment_units = 0U;
        std::uint64_t limit_units = 0U;
        if (!parseUnsignedDecimalToScale(
                rules.price_increment, rules.price_scale, &increment_units) ||
            increment_units == 0U ||
            !doubleToScaledUnitsExact(limit, rules.price_scale, &limit_units))
            throw std::runtime_error("MOCK price grid unavailable during matching");

        long double factor = 1.0L;
        for (std::size_t i = 0; i < rules.price_scale; ++i)
            factor *= 10.0L;

        const long double raw_units = candidate * factor;
        std::uint64_t candidate_units = buy
            ? static_cast<std::uint64_t>(std::ceil(raw_units - 1e-12L))
            : static_cast<std::uint64_t>(std::floor(raw_units + 1e-12L));

        if (buy) {
            candidate_units =
                ((candidate_units + increment_units - 1U) / increment_units) *
                increment_units;
            candidate_units = std::min(candidate_units, limit_units);
        } else {
            candidate_units =
                (candidate_units / increment_units) * increment_units;
            candidate_units = std::max(candidate_units, limit_units);
        }

        const double result = scaledUnitsToDouble(
            candidate_units, rules.price_scale);
        if (!std::isfinite(result) || result <= 0.0)
            throw std::runtime_error("invalid MOCK synthetic fill price");
        return result;
    }

    void forwardLifecycleTransition(
        OrderID order_id,
        Timestamp timestamp,
        OrderLifecycleStatus next_status,
        double cumulative,
        double remaining,
        const std::string& native_status,
        const Error& reason)
    {
        const std::size_t before = lifecycle_.emittedEvents().size();
        if (!lifecycle_.applyExternalLifecycleTransition(
                order_id,
                timestamp,
                next_status,
                cumulative,
                remaining,
                native_status,
                reason))
            throw std::runtime_error("invalid Step51 lifecycle transition");

        const auto& source = lifecycle_.emittedEvents();
        if (source.size() != before + 1U)
            throw std::runtime_error("unexpected Step50 lifecycle emission count");
        emit(source.back());
    }

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
