#include <cassert>
#include <cmath>
#include <filesystem>
#include <string>
#include <variant>

#include <unistd.h>

#include "mock_fault_chaos_rate_limit_v1.h"

using namespace VenueContracts::V1;
using namespace MockVenueV1;

namespace {

std::filesystem::path tempDir(const std::string& suffix)
{
    return std::filesystem::temp_directory_path() /
        ("algoTrading-step55-" +
         std::to_string(::getpid()) + "-" + suffix);
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
    const auto* entry =
        findByCanonicalAssetExact(asset);
    assert(entry != nullptr);

    LimitOrderIntent o;
    o.local_order_id = id;
    o.strategy_id = 1;
    o.created_at = 1;
    o.active_from = 2;
    o.instrument = entry->instrument();
    o.side = Side::Buy;
    o.quantity = quantity;
    o.limit_price = price;
    o.time_in_force = TimeInForce::Gtc;
    o.client_order_id =
        "chaos-" + std::to_string(id);
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
        SubmitOrderItem{
            "item-" + request_id,
            order
        });
    return b;
}

CancelOrderBatch cancelOne(
    const std::string& request_id,
    const StoredOrder& stored,
    Timestamp ts)
{
    CancelOrderBatch b;
    b.request = req(request_id, ts);

    CancelOrderItem item;
    item.item_id =
        "item-" + request_id;
    item.order.local_order_id =
        stored.intent.local_order_id;
    item.order.instrument =
        stored.intent.instrument;
    item.order.native_references =
        stored.native_references;

    b.items.push_back(item);
    return b;
}

MarketBarObservationV1 bar(
    const std::string& asset,
    Timestamp ts,
    double open = 100.0,
    double high = 101.0,
    double low = 99.0,
    double close = 100.0,
    double volume = 100.0)
{
    MarketBarObservationV1 b;
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

void cleanReconcile(
    MockFaultChaosRateLimitV1& engine,
    Timestamp ts)
{
    const auto r = engine.reconcile(ts);
    assert(r.clean());
    assert(engine.canRouteNewSubmit());
}

struct CampaignResult {
    std::string economic;
    std::string stream;
    std::string evidence;
};

CampaignResult deterministicCampaign(
    const std::filesystem::path& dir,
    std::uint64_t seed)
{
    MockChaosConfigV1 config;
    config.seed = seed;
    config.auto_submit_faults = true;
    config.submit_limit = 100U;
    config.reconcile_limit = 100U;
    config.market_source_limit = 100U;

    MockFaultChaosRateLimitV1 engine(
        dir,
        config);

    OrderID next_order = 5000U;
    Timestamp time = 1000U;

    for (int i = 0; i < 8; ++i) {
        cleanReconcile(engine, time++);

        const auto order =
            makeOrder(
                next_order,
                "BTCUSDT",
                0.1,
                100.0);

        const auto result =
            engine.submit(
                submitOne(
                    "auto-" + std::to_string(i),
                    order,
                    time++));

        if (result.status ==
                ChaosStatusV1::Delivered ||
            result.status ==
                ChaosStatusV1::DelayedResponse ||
            result.status ==
                ChaosStatusV1::LostResponseAmbiguous) {
            ++next_order;
        } else {
            assert(
                result.status ==
                ChaosStatusV1::CanonicalRejected);
        }

        // The venue can execute accepted orders irrespective of response delivery.
        const auto source =
            engine.processBar(
                bar("BTCUSDT", time++));
        assert(
            source.status ==
                ChaosStatusV1::Delivered ||
            source.status ==
                ChaosStatusV1::DuplicateIgnored);

        // Resolve any ambiguity/staleness before the next new-exposure attempt.
        cleanReconcile(engine, time++);
    }

    return {
        engine.runtime().account().economicFingerprint(),
        engine.runtime().streamFingerprint(),
        engine.evidenceFingerprint()
    };
}

} // namespace

int main()
{
    std::error_code ignored;

    // -----------------------------------------------------------------
    // Canonical reject, lost response, delayed response, idempotent retry.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("command-faults");
        std::filesystem::remove_all(dir, ignored);

        MockChaosConfigV1 config;
        config.submit_limit = 100U;
        config.reconcile_limit = 100U;
        MockFaultChaosRateLimitV1 engine(dir, config);
        cleanReconcile(engine, 10);

        const auto reject_order =
            makeOrder(1001, "BTCUSDT", 0.1, 100.0);
        const auto seq_before_reject =
            engine.runtime().lastSequence();

        engine.injectNext(
            ChaosFaultV1::CanonicalReject);
        const auto rejected =
            engine.submit(
                submitOne(
                    "reject-1",
                    reject_order,
                    11));

        assert(
            rejected.status ==
            ChaosStatusV1::CanonicalRejected);
        assert(
            rejected.error.classification ==
            ErrorClass::VenueLimit);
        assert(
            engine.runtime().lastSequence() ==
            seq_before_reject);
        assert(
            engine.runtime().lifecycle().
                findOrder(1001) == nullptr);

        const auto lost_order =
            makeOrder(1002, "BTCUSDT", 0.1, 100.0);

        engine.injectNext(
            ChaosFaultV1::LoseResponseAmbiguousSubmit);
        const auto lost =
            engine.submit(
                submitOne(
                    "lost-1",
                    lost_order,
                    12));

        assert(
            lost.status ==
            ChaosStatusV1::LostResponseAmbiguous);
        assert(lost.underlying_may_have_applied);
        assert(lost.requires_reconciliation_before_retry);
        assert(
            engine.runtime().lifecycle().
                findOrder(1002) != nullptr);
        assert(!engine.canRouteNewSubmit());

        cleanReconcile(engine, 13);

        const std::size_t orders_before_retry =
            engine.runtime().lifecycle().orders().size();
        const auto retry =
            engine.submit(
                submitOne(
                    "lost-1",
                    lost_order,
                    12));
        assert(
            retry.status ==
                ChaosStatusV1::Delivered ||
            retry.status ==
                ChaosStatusV1::DuplicateIgnored);
        assert(
            engine.runtime().lifecycle().orders().size() ==
            orders_before_retry);

        cleanReconcile(engine, 14);

        const auto delayed_order =
            makeOrder(1003, "BTCUSDT", 0.1, 100.0);

        engine.injectNext(
            ChaosFaultV1::DelayResponse);
        const auto delayed =
            engine.submit(
                submitOne(
                    "delayed-1",
                    delayed_order,
                    15));
        assert(
            delayed.status ==
            ChaosStatusV1::DelayedResponse);
        assert(
            engine.runtime().lifecycle().
                findOrder(1003) != nullptr);

        const auto released =
            engine.releaseDelayedResponses();
        assert(released.size() == 1U);
        assert(released[0].command_key == "delayed-1");
        assert(
            released[0].underlying ==
            RecoverySourceResultV1::Applied);

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // Disconnect/reconnect: new submit blocked; cancel still available.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("disconnect");
        std::filesystem::remove_all(dir, ignored);

        MockFaultChaosRateLimitV1 engine(dir);
        cleanReconcile(engine, 100);

        const auto live_order =
            makeOrder(2001, "ETHUSDT", 0.01, 3000.0);
        const auto submit_result =
            engine.submit(
                submitOne(
                    "live-1",
                    live_order,
                    101));
        assert(
            submit_result.status ==
            ChaosStatusV1::Delivered);

        cleanReconcile(engine, 102);

        engine.setUserStreamConnected(false, 103);
        assert(!engine.canRouteNewSubmit());

        const auto blocked =
            engine.submit(
                submitOne(
                    "blocked-disconnect",
                    makeOrder(
                        2002,
                        "SOLUSDT",
                        0.1,
                        150.0),
                    104));
        assert(
            blocked.status ==
            ChaosStatusV1::UserStreamDisconnected);

        const StoredOrder* stored =
            engine.runtime().lifecycle().
                findOrder(2001);
        assert(stored != nullptr);

        const auto cancel_result =
            engine.cancel(
                cancelOne(
                    "cancel-while-disconnected",
                    *stored,
                    105));
        assert(
            cancel_result.status ==
            ChaosStatusV1::Delivered);

        const auto unavailable_page =
            engine.streamAfter(0U, 10U, 106);
        assert(!unavailable_page.available);

        engine.setUserStreamConnected(true, 107);
        assert(!engine.canRouteNewSubmit());

        const auto backfill =
            engine.streamAfter(0U, 256U, 108);
        assert(backfill.available);
        assert(!backfill.page.events.empty());

        cleanReconcile(engine, 109);

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // Stale snapshot => PENDING; venue unavailable => PENDING/block.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("snapshot-availability");
        std::filesystem::remove_all(dir, ignored);

        MockFaultChaosRateLimitV1 engine(dir);
        cleanReconcile(engine, 200);

        engine.injectNext(
            ChaosFaultV1::StaleSnapshot);
        const auto stale =
            engine.reconcile(201);
        assert(!stale.clean());
        assert(
            stale.state ==
            ReconciliationStateV1::Pending);
        assert(!engine.canRouteNewSubmit());

        cleanReconcile(engine, 202);

        engine.setVenueAvailable(false, 203);
        const auto pending =
            engine.reconcile(204);
        assert(
            pending.state ==
            ReconciliationStateV1::Pending);

        const auto unavailable_submit =
            engine.submit(
                submitOne(
                    "venue-down",
                    makeOrder(
                        3001,
                        "BTCUSDT",
                        0.1,
                        100.0),
                    205));
        assert(
            unavailable_submit.status ==
            ChaosStatusV1::VenueUnavailable);

        engine.setVenueAvailable(true, 206);
        assert(!engine.canRouteNewSubmit());
        cleanReconcile(engine, 207);

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // Deterministic fixed-window rate limiting and recovery.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("rate-limit");
        std::filesystem::remove_all(dir, ignored);

        MockChaosConfigV1 config;
        config.submit_limit = 1U;
        config.reconcile_limit = 100U;

        MockFaultChaosRateLimitV1 engine(dir, config);
        cleanReconcile(engine, 300);

        const auto first =
            engine.submit(
                submitOne(
                    "rate-1",
                    makeOrder(
                        4001,
                        "BTCUSDT",
                        0.1,
                        100.0),
                    301));
        assert(
            first.status ==
            ChaosStatusV1::Delivered);

        cleanReconcile(engine, 302);

        const auto limited =
            engine.submit(
                submitOne(
                    "rate-2",
                    makeOrder(
                        4002,
                        "BTCUSDT",
                        0.1,
                        100.0),
                    303));
        assert(
            limited.status ==
            ChaosStatusV1::RateLimited);
        assert(
            limited.error.classification ==
            ErrorClass::RateLimited);
        assert(limited.error.retryable);

        // 311 is a new 10-unit event-time window.
        const auto recovered =
            engine.submit(
                submitOne(
                    "rate-2",
                    makeOrder(
                        4002,
                        "BTCUSDT",
                        0.1,
                        100.0),
                    311));
        assert(
            recovered.status ==
            ChaosStatusV1::Delivered);

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // Duplicate source, duplicate/out-of-order user delivery.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("delivery-faults");
        std::filesystem::remove_all(dir, ignored);

        MockFaultChaosRateLimitV1 engine(dir);
        cleanReconcile(engine, 400);

        assert(
            engine.submit(
                submitOne(
                    "delivery-order",
                    makeOrder(
                        5001,
                        "BTCUSDT",
                        0.1,
                        100.0),
                    401)).status ==
            ChaosStatusV1::Delivered);

        engine.injectNext(
            ChaosFaultV1::DuplicateSource);
        const auto duplicate_source =
            engine.processBar(
                bar("BTCUSDT", 402));
        assert(
            duplicate_source.status ==
            ChaosStatusV1::Delivered);
        assert(engine.runtime().recoverySafe());

        engine.injectNext(
            ChaosFaultV1::DuplicateUserDelivery);
        const auto duplicate_delivery =
            engine.streamAfter(0U, 64U, 403);
        assert(duplicate_delivery.available);
        assert(
            duplicate_delivery.page.events.size() >= 2U);
        assert(
            duplicate_delivery.page.events[0].sequence ==
            duplicate_delivery.page.events[1].sequence);

        engine.injectNext(
            ChaosFaultV1::OutOfOrderUserDelivery);
        const auto out_of_order_delivery =
            engine.streamAfter(0U, 64U, 404);
        assert(out_of_order_delivery.available);
        assert(
            out_of_order_delivery.page.events.size() >= 2U);
        assert(
            out_of_order_delivery.page.events[0].sequence >
            out_of_order_delivery.page.events[1].sequence);

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // New out-of-order source fails closed and makes recovery unsafe.
    // -----------------------------------------------------------------
    {
        const auto dir = tempDir("out-of-order-source");
        std::filesystem::remove_all(dir, ignored);

        MockFaultChaosRateLimitV1 engine(dir);
        cleanReconcile(engine, 500);

        assert(
            engine.submit(
                submitOne(
                    "ooo-order",
                    makeOrder(
                        6001,
                        "ETHUSDT",
                        0.01,
                        3000.0),
                    501)).status ==
            ChaosStatusV1::Delivered);

        assert(
            engine.processBar(
                bar(
                    "ETHUSDT",
                    510,
                    3000.0,
                    3010.0,
                    2990.0,
                    3005.0,
                    100.0)).status ==
            ChaosStatusV1::Delivered);

        engine.injectNext(
            ChaosFaultV1::OutOfOrderSource);
        const auto out_of_order =
            engine.processBar(
                bar(
                    "SOLUSDT",
                    520,
                    150.0,
                    151.0,
                    149.0,
                    150.0,
                    100.0));

        assert(
            out_of_order.status ==
            ChaosStatusV1::OutOfOrderRejected);
        assert(!engine.runtime().recoverySafe());
        assert(!engine.canRouteNewSubmit());

        std::filesystem::remove_all(dir, ignored);
    }

    // -----------------------------------------------------------------
    // Same seed + same inputs => same economics, stream and chaos evidence.
    // -----------------------------------------------------------------
    {
        const auto dir_a = tempDir("determinism-a");
        const auto dir_b = tempDir("determinism-b");
        std::filesystem::remove_all(dir_a, ignored);
        std::filesystem::remove_all(dir_b, ignored);

        const CampaignResult a =
            deterministicCampaign(dir_a, 550055ULL);
        const CampaignResult b =
            deterministicCampaign(dir_b, 550055ULL);

        assert(a.economic == b.economic);
        assert(a.stream == b.stream);
        assert(a.evidence == b.evidence);

        std::filesystem::remove_all(dir_a, ignored);
        std::filesystem::remove_all(dir_b, ignored);
    }

    return 0;
}
