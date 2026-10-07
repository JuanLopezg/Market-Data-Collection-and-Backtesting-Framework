#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

#include <unistd.h>

#include "mock/recovery.h"

using namespace VenueContracts::V1;
using namespace MockVenue;

namespace {

bool near(double a, double b, double eps = 1e-8)
{
    return std::abs(a - b) <= eps;
}

RequestIdentity req(const std::string& id, Timestamp ts)
{
    RequestIdentity r;
    r.venue = context();
    r.request_id = id;
    r.correlation_id = "corr-" + id;
    r.requested_at = ts;
    return r;
}

LimitOrderIntent makeOrder(
    OrderID id,
    const std::string& asset,
    double quantity,
    double price)
{
    const auto* entry = findByCanonicalAssetExact(asset);
    assert(entry != nullptr);

    LimitOrderIntent o;
    o.local_order_id = id;
    o.strategy_id = 1;
    o.created_at = 10;
    o.active_from = 11;
    o.instrument = entry->instrument();
    o.side = Side::Buy;
    o.quantity = quantity;
    o.limit_price = price;
    o.time_in_force = TimeInForce::Gtc;
    o.client_order_id = "recover-" + std::to_string(id);
    return o;
}

SubmitOrderBatch submitOne(
    const std::string& request_id,
    const LimitOrderIntent& order,
    Timestamp ts)
{
    SubmitOrderBatch b;
    b.request = req(request_id, ts);
    b.items.push_back(
        SubmitOrderItem{"item-" + request_id, order});
    return b;
}

MarketBarObservation bar(
    const std::string& asset,
    Timestamp ts,
    double open,
    double high,
    double low,
    double close,
    double volume)
{
    MarketBarObservation b;
    b.canonical_asset = asset;
    b.event_time = ts;
    b.bar.open = open;
    b.bar.high = high;
    b.bar.low = low;
    b.bar.close = close;
    b.bar.volume = volume;
    assert(b.valid());
    return b;
}

} // namespace

int main()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("algoTrading-step53-" + std::to_string(::getpid()));

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    std::uint64_t checkpoint_sequence = 0U;
    std::uint64_t final_sequence = 0U;
    std::string final_economic_fingerprint;
    std::string final_stream_fingerprint;
    double final_equity = 0.0;

    {
        MockRecovery runtime(dir);

        assert(runtime.recoverySafe());
        assert(runtime.lastSequence() == 0U);
        assert(runtime.lastBusinessEventTime() == 0U);

        assert(runtime.setLeverage("BTCUSDT", 90, 2U) ==
               RecoverySourceResult::Applied);

        const auto order =
            makeOrder(1001, "BTCUSDT", 0.20, 100.0);
        assert(runtime.submit(
            submitOne("submit-1001", order, 100)) ==
            RecoverySourceResult::Applied);

        // First bar has 1.0 volume, 10% participation => 0.1 fill.
        const auto first_bar =
            bar("BTCUSDT", 110, 101.0, 102.0, 99.0, 100.0, 1.0);
        assert(runtime.processBar(first_bar) ==
               RecoverySourceResult::Applied);

        const auto snapshot_before_checkpoint =
            runtime.stateSnapshot(110);
        assert(snapshot_before_checkpoint.snapshot_sequence ==
               runtime.lastSequence());
        assert(snapshot_before_checkpoint.open_orders.orders.size() == 1U);
        assert(snapshot_before_checkpoint.fills.fills.size() == 1U);
        assert(snapshot_before_checkpoint.account.positions.size() == 1U);
        assert(near(
            snapshot_before_checkpoint.account.positions[0].signed_quantity,
            0.1));

        assert(runtime.checkpoint());
        checkpoint_sequence = runtime.lastCheckpointSequence();
        assert(checkpoint_sequence == runtime.lastSequence());
        assert(checkpoint_sequence > 0U);

        // Exact duplicate old market observation after checkpoint is ignored,
        // even though it is older/equal to future events later.
        const std::uint64_t before_duplicate_bar =
            runtime.lastSequence();
        const std::string before_duplicate_bar_econ =
            runtime.account().economicFingerprint();
        assert(runtime.processBar(first_bar) ==
               RecoverySourceResult::DuplicateIgnored);
        assert(runtime.lastSequence() == before_duplicate_bar);
        assert(runtime.account().economicFingerprint() ==
               before_duplicate_bar_econ);

        // Second bar completes the remaining 0.1.
        const auto second_bar =
            bar("BTCUSDT", 120, 101.0, 103.0, 98.0, 101.0, 1.0);
        assert(runtime.processBar(second_bar) ==
               RecoverySourceResult::Applied);

        assert(runtime.postFundingPayment(
            "BTCUSDT",
            121,
            "-0.25000000",
            "funding-121") ==
            RecoverySourceResult::Applied);

        assert(runtime.postRebate(
            "BTCUSDT",
            122,
            "0.10000000",
            "rebate-122") ==
            RecoverySourceResult::Applied);

        assert(runtime.markToMarket(
            "BTCUSDT",
            123,
            105.0) ==
            RecoverySourceResult::Applied);

        const auto final_snapshot =
            runtime.stateSnapshot(123);
        assert(final_snapshot.open_orders.orders.empty());
        assert(final_snapshot.fills.fills.size() == 2U);
        assert(final_snapshot.account.positions.size() == 1U);
        assert(near(
            final_snapshot.account.positions[0].signed_quantity,
            0.2));
        assert(near(
            final_snapshot.account.positions[0].mark_price,
            105.0));

        final_sequence = runtime.lastSequence();
        final_economic_fingerprint =
            runtime.account().economicFingerprint();
        final_stream_fingerprint =
            runtime.streamFingerprint();
        final_equity =
            final_snapshot.account.equity;

        assert(final_sequence > checkpoint_sequence);

        // Bounded snapshot+incremental merge contract.
        std::uint64_t cursor = checkpoint_sequence;
        std::uint64_t expected = checkpoint_sequence + 1U;
        std::size_t recovered_incrementals = 0U;

        while (cursor < final_sequence) {
            const auto page =
                runtime.streamAfter(cursor, 2U);
            assert(!page.events.empty());
            assert(page.events.size() <= 2U);

            for (const auto& envelope : page.events) {
                assert(envelope.sequence == expected);
                ++expected;
                ++recovered_incrementals;
            }

            assert(page.next_cursor == page.events.back().sequence);
            cursor = page.next_cursor;
        }

        assert(cursor == final_sequence);
        assert(recovered_incrementals ==
               static_cast<std::size_t>(
                   final_sequence - checkpoint_sequence));

        const auto fill_page =
            runtime.fillsAfter(checkpoint_sequence, 10U, 123);
        assert(fill_page.fills.size() == 1U); // second fill only
        assert(fill_page.fills[0].native_references.native_fill_id ==
               "mock-fill-1001-2");
    }

    // Restart: checkpoint + incremental journal reconstruct exact state and stream.
    {
        MockRecovery recovered(dir);

        assert(recovered.recoverySafe());
        assert(recovered.lastCheckpointSequence() == checkpoint_sequence);
        assert(recovered.lastSequence() == final_sequence);
        assert(recovered.account().economicFingerprint() ==
               final_economic_fingerprint);
        assert(recovered.streamFingerprint() ==
               final_stream_fingerprint);

        const auto state =
            recovered.stateSnapshot(123);
        assert(near(state.account.equity, final_equity));
        assert(state.open_orders.orders.empty());
        assert(state.fills.fills.size() == 2U);

        // Request-id cache was reconstructed. Same request + same payload returns
        // cached OperationResult only; it cannot create a second order.
        const std::uint64_t before_retry =
            recovered.lastSequence();
        const auto order =
            makeOrder(1001, "BTCUSDT", 0.20, 100.0);
        assert(recovered.submit(
            submitOne("submit-1001", order, 100)) ==
            RecoverySourceResult::Applied);
        assert(recovered.lifecycle().orders().size() == 1U);
        assert(recovered.lastSequence() == before_retry + 1U);
        assert(std::holds_alternative<OperationResult>(
            recovered.stream().back().event));

        // Duplicate market source and duplicate funding remain idempotent after restart.
        const auto second_bar =
            bar("BTCUSDT", 120, 101.0, 103.0, 98.0, 101.0, 1.0);

        const std::uint64_t before_dupes =
            recovered.lastSequence();
        const std::string before_dupes_econ =
            recovered.account().economicFingerprint();

        assert(recovered.processBar(second_bar) ==
               RecoverySourceResult::DuplicateIgnored);
        assert(recovered.postFundingPayment(
            "BTCUSDT",
            121,
            "-0.25000000",
            "funding-121") ==
            RecoverySourceResult::DuplicateIgnored);

        assert(recovered.lastSequence() == before_dupes);
        assert(recovered.account().economicFingerprint() ==
               before_dupes_econ);

        // New out-of-order source input fails closed and does not mutate economics.
        const auto old_eth_bar =
            bar("ETHUSDT", 119, 3000.0, 3010.0, 2990.0, 3005.0, 10.0);

        const std::uint64_t before_ooo =
            recovered.lastSequence();
        const std::string before_ooo_econ =
            recovered.account().economicFingerprint();

        assert(recovered.processBar(old_eth_bar) ==
               RecoverySourceResult::OutOfOrderRejected);
        assert(!recovered.recoverySafe());
        assert(recovered.lastSequence() == before_ooo);
        assert(recovered.account().economicFingerprint() ==
               before_ooo_econ);

        // Unsafe state may not overwrite the last good checkpoint.
        assert(!recovered.checkpoint());
    }

    // The rejected out-of-order input was never persisted, so a clean restart returns
    // to the last durable safe state (plus the durable request retry).
    {
        MockRecovery recovered_again(dir);
        assert(recovered_again.recoverySafe());
        assert(recovered_again.account().economicFingerprint() ==
               final_economic_fingerprint);
        assert(recovered_again.lastSequence() == final_sequence + 1U);

        // The extra durable event is only the retried cached OperationResult.
        assert(std::holds_alternative<OperationResult>(
            recovered_again.stream().back().event));
    }

    std::filesystem::remove_all(dir, ignored);
    return 0;
}
