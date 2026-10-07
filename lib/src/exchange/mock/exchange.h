#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "canonical_venue_adapter.h"
#include "faults.h"

// Expose the complete deterministic MOCK venue through CanonicalVenueAdapter.
//
// Higher-level trading code talks only to the canonical adapter contract. This facade
// forwards commands into the MOCK lifecycle/matching/account/recovery pipeline and emits
// canonical venue events back to the caller.

namespace MockVenue {

class MockExchange final : public CanonicalVenueAdapter {
public:
    explicit MockExchange(
        std::filesystem::path durable_directory,
        MockChaosConfig chaos_config = {},
        MatchingFillConfig matching_config = {},
        MockAccountingConfig accounting_config = {})
        : chaos_(
            std::move(durable_directory),
            std::move(chaos_config),
            std::move(matching_config),
            std::move(accounting_config))
    {
        forwarded_sequence_ = chaos_.runtime().lastSequence();
    }

    VenueContracts::V1::VenueContext context() const override
    {
        return MockVenue::context();
    }

    VenueContracts::V1::CapabilitySet capabilities() const override
    {
        using VenueContracts::V1::Capability;
        return VenueContracts::V1::CapabilitySet{
            Capability::MarketMetadata,
            Capability::TradingRules,
            Capability::SubmitOrder,
            Capability::CancelOrder,
            Capability::ModifyOrder,
            Capability::ClientOrderId,
            Capability::IdempotentSubmit,
            Capability::TimeInForceGtc,
            Capability::TimeInForceIoc,
            Capability::PostOnly,
            Capability::ReduceOnly,
            Capability::BatchActions,
            Capability::Leverage,
            Capability::MarginState,
            Capability::AccountSnapshot,
            Capability::OpenOrders,
            Capability::HistoricalOrders,
            Capability::Fills,
            Capability::UserStream,
            Capability::SnapshotBackfill,
            Capability::RateLimitIntrospection,
            Capability::OrderStatusQuery,
            Capability::FeeAccounting,
            Capability::FundingAccounting
        };
    }

    void setEventHandler(EventHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void submitOrders(
        const VenueContracts::V1::SubmitOrderBatch& request) override
    {
        const auto result = chaos_.submit(request);

        if (result.status == ChaosStatus::LostResponseAmbiguous) {
            flushNewEvents(
                request.request.request_id,
                FlushOperationResultMode::SuppressMatching);
            return;
        }

        if (result.status == ChaosStatus::DelayedResponse) {
            flushNewEvents(
                request.request.request_id,
                FlushOperationResultMode::HoldMatching);
            return;
        }

        const std::uint64_t before = forwarded_sequence_;
        flushNewEvents({}, FlushOperationResultMode::ForwardAll);

        if (forwarded_sequence_ == before &&
            !result.error.none()) {
            emit(syntheticOperationFailure(
                request.request,
                VenueContracts::V1::CommandKind::Submit,
                result.error));
        }
    }

    void cancelOrders(
        const VenueContracts::V1::CancelOrderBatch& request) override
    {
        const auto result = chaos_.cancel(request);

        const std::uint64_t before = forwarded_sequence_;
        flushNewEvents({}, FlushOperationResultMode::ForwardAll);

        if (forwarded_sequence_ == before &&
            !result.error.none()) {
            emit(syntheticOperationFailure(
                request.request,
                VenueContracts::V1::CommandKind::Cancel,
                result.error));
        }
    }

    void modifyOrders(
        const VenueContracts::V1::ModifyOrderBatch& request) override
    {
        const auto result = chaos_.modify(request);

        const std::uint64_t before = forwarded_sequence_;
        flushNewEvents({}, FlushOperationResultMode::ForwardAll);

        if (forwarded_sequence_ == before &&
            !result.error.none()) {
            emit(syntheticOperationFailure(
                request.request,
                VenueContracts::V1::CommandKind::Modify,
                result.error));
        }
    }

    void requestAccountSnapshot(
        const VenueContracts::V1::AccountSnapshotRequest& request) override
    {
        if (!request.valid())
            return;
        emit(
            chaos_.runtime().account().accountSnapshot(
                request.request.requested_at));
    }

    void requestOpenOrders(
        const VenueContracts::V1::OpenOrdersRequest& request) override
    {
        if (!request.valid())
            return;
        emit(
            chaos_.runtime().account().openOrdersSnapshot(
                chaos_.runtime().lifecycle(),
                request.request.requested_at));
    }

    void requestOrderStatus(
        const VenueContracts::V1::OrderStatusRequest& request) override
    {
        if (!request.valid())
            return;

        VenueContracts::V1::OrderUpdate update;
        update.venue = context();
        update.instrument = request.order.instrument;
        update.causal_request_id = request.request.request_id;
        update.local_order_id = request.order.local_order_id;
        update.timestamp = request.request.requested_at;

        const StoredOrder* stored =
            chaos_.runtime().lifecycle().findOrder(
                request.order.local_order_id);

        if (stored == nullptr) {
            update.status =
                VenueContracts::V1::OrderLifecycleStatus::
                    UnknownRequiresReconciliation;
            update.native_references =
                request.order.native_references;
            update.native_status =
                "ORDER_STATUS_NOT_FOUND";
            update.terminal_reason.classification =
                VenueContracts::V1::ErrorClass::OrderNotFound;
            update.terminal_reason.retryable = false;
            update.terminal_reason.native_code =
                "MOCK_V1";
            update.terminal_reason.native_reason =
                "ORDER_STATUS_NOT_FOUND";
            update.terminal_reason.detail =
                "order status query did not find local MOCK order";
            emit(update);
            return;
        }

        update.instrument = stored->intent.instrument;
        update.status = stored->status;
        update.cumulative_filled_quantity =
            stored->cumulative_filled_quantity;
        update.remaining_quantity =
            stored->remaining_quantity;
        update.native_references =
            stored->native_references;
        update.native_status =
            VenueContracts::V1::toString(stored->status);
        emit(update);
    }

    void requestFills(
        const VenueContracts::V1::FillBackfillRequest& request) override
    {
        if (!request.valid())
            return;

        VenueContracts::V1::FillBatch batch;
        batch.venue = context();
        batch.produced_at =
            request.request.requested_at;
        batch.is_snapshot = false;

        for (const auto& envelope :
             chaos_.runtime().stream()) {
            if (!std::holds_alternative<
                    VenueContracts::V1::Fill>(
                    envelope.event))
                continue;

            const auto& fill =
                std::get<VenueContracts::V1::Fill>(
                    envelope.event);

            if (fill.timestamp <
                    request.from_inclusive ||
                fill.timestamp >
                    request.to_inclusive)
                continue;

            batch.fills.push_back(fill);
            batch.next_cursor =
                std::to_string(envelope.sequence);
        }

        emit(batch);
    }

    void poll(int timeout_ms) override
    {
        (void)timeout_ms;

        // Explicit deterministic release. No wall-clock timer is consulted.
        for (const auto& event : held_operation_results_)
            emit(event);
        held_operation_results_.clear();
        (void)chaos_.releaseDelayedResponses();
    }

    // MOCK replay control plane.
    //
    // These methods are intentionally NOT part of CanonicalVenueAdapter.
    // Strategy/Risk/Planner therefore cannot call them through the trading
    // boundary.
    ChaosOperationResult processMarketBar(
        const MarketBarObservation& observation)
    {
        const auto result =
            chaos_.processBar(observation);
        flushNewEvents(
            {},
            FlushOperationResultMode::ForwardAll);
        return result;
    }

    RecoverySourceResult markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price)
    {
        return chaos_.runtime().markToMarket(
            canonical_asset,
            event_time,
            mark_price);
    }

    ReconciliationReport reconcile(
        Timestamp snapshot_time)
    {
        return chaos_.reconcile(snapshot_time);
    }

    bool canRouteNewSubmit() const
    {
        return chaos_.canRouteNewSubmit();
    }

    MockFaults& chaos()
    {
        return chaos_;
    }

    const MockFaults& chaos() const
    {
        return chaos_;
    }

private:
    enum class FlushOperationResultMode {
        ForwardAll = 0,
        SuppressMatching,
        HoldMatching
    };

    MockFaults chaos_;
    EventHandler handler_;
    std::uint64_t forwarded_sequence_ = 0U;
    std::vector<VenueContracts::V1::Event>
        held_operation_results_;

    static VenueContracts::V1::OperationResult
    syntheticOperationFailure(
        const VenueContracts::V1::RequestIdentity& request,
        VenueContracts::V1::CommandKind command,
        const VenueContracts::V1::Error& error)
    {
        VenueContracts::V1::OperationResult result;
        result.request = request;
        result.command = command;
        result.scope =
            VenueContracts::V1::ResultScope::Operation;
        result.accepted = false;
        result.operation_error = error;
        return result;
    }

    void flushNewEvents(
        const std::string& matching_request_id,
        FlushOperationResultMode mode)
    {
        for (const auto& envelope :
             chaos_.runtime().stream()) {
            if (envelope.sequence <=
                forwarded_sequence_)
                continue;

            bool matching_operation = false;
            if (std::holds_alternative<
                    VenueContracts::V1::OperationResult>(
                    envelope.event)) {
                const auto& result =
                    std::get<
                        VenueContracts::V1::OperationResult>(
                        envelope.event);

                matching_operation =
                    !matching_request_id.empty() &&
                    result.request.request_id ==
                        matching_request_id;
            }

            if (matching_operation &&
                mode ==
                    FlushOperationResultMode::
                        HoldMatching) {
                held_operation_results_.push_back(
                    envelope.event);
            } else if (
                !(matching_operation &&
                  mode ==
                    FlushOperationResultMode::
                        SuppressMatching)) {
                emit(envelope.event);
            }

            forwarded_sequence_ =
                envelope.sequence;
        }
    }

    template <typename T>
    void emit(const T& value)
    {
        if (!handler_)
            return;
        VenueContracts::V1::Event event = value;
        handler_(event);
    }
};

} // namespace MockVenue
