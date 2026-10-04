#include <cassert>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include "mock_deterministic_matching_fill_v1.h"

using namespace VenueContracts::V1;
using namespace MockVenueV1;

namespace {

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
    Side side,
    double quantity,
    double limit,
    TimeInForce tif = TimeInForce::Gtc,
    bool post_only = false)
{
    const auto* entry = findByCanonicalAssetExact(asset);
    assert(entry != nullptr);
    LimitOrderIntent o;
    o.local_order_id = id;
    o.strategy_id = 7;
    o.created_at = 10;
    o.active_from = 20;
    o.instrument = entry->instrument();
    o.side = side;
    o.quantity = quantity;
    o.limit_price = limit;
    o.time_in_force = tif;
    o.post_only = post_only;
    o.client_order_id = "cloid-" + std::to_string(id);
    return o;
}

void submit(
    MockOrderAdmissionLifecycleV1& life,
    const std::string& request_id,
    const LimitOrderIntent& order,
    Timestamp ts = 11)
{
    SubmitOrderBatch b;
    b.request = req(request_id, ts);
    b.items.push_back(SubmitOrderItem{"item-" + request_id, order});
    life.submit(b);
    assert(life.findOrder(order.local_order_id) != nullptr);
    life.clearEmittedEvents();
}

MarketBarObservationV1 bar(
    const std::string& asset,
    Timestamp ts,
    double open,
    double high,
    double low,
    double close,
    double volume)
{
    MarketBarObservationV1 x;
    x.canonical_asset = asset;
    x.event_time = ts;
    x.bar.open = open;
    x.bar.high = high;
    x.bar.low = low;
    x.bar.close = close;
    x.bar.volume = volume;
    return x;
}

const Fill& fillAt(const std::vector<Event>& events, std::size_t i)
{
    assert(i < events.size());
    assert(std::holds_alternative<Fill>(events[i]));
    return std::get<Fill>(events[i]);
}

const OrderUpdate& updateAt(const std::vector<Event>& events, std::size_t i)
{
    assert(i < events.size());
    assert(std::holds_alternative<OrderUpdate>(events[i]));
    return std::get<OrderUpdate>(events[i]);
}

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a-b) <= eps;
}

} // namespace

int main()
{
    // 1) Full GTC fill: Fill is economic authority and precedes FILLED update.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s1", makeOrder(1, "BTCUSDT", Side::Buy, 0.001, 50000.0));
        MockDeterministicMatchingFillV1 matcher(life);
        matcher.processBar(bar("BTCUSDT", 30, 49900.0, 50100.0, 49800.0, 50050.0, 1.0));
        assert(matcher.emittedEvents().size() == 2U);
        const auto& f = fillAt(matcher.emittedEvents(), 0);
        assert(f.local_order_id == 1U);
        assert(f.native_references.native_fill_id == "mock-fill-1-1");
        assert(f.quantity == 0.001);
        assert(f.price <= 50000.0);
        assert(f.timestamp == 30U);
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::Filled);
        assert(life.findOrder(1)->status == OrderLifecycleStatus::Filled);
        assert(matcher.traces().size() == 1U);
        assert(matcher.traces()[0].diagnostic_fee_quote > 0.0);
        for (const auto& e : matcher.emittedEvents())
            assert(!std::holds_alternative<AccountingEvent>(e));
    }

    // 2) GTC no touch -> RESTING, no Fill.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s2", makeOrder(2, "BTCUSDT", Side::Buy, 0.001, 49000.0));
        MockDeterministicMatchingFillV1 matcher(life);
        matcher.processBar(bar("BTCUSDT", 30, 50000.0, 50100.0, 49500.0, 49900.0, 1.0));
        assert(matcher.emittedEvents().size() == 1U);
        assert(updateAt(matcher.emittedEvents(), 0).status == OrderLifecycleStatus::Resting);
        assert(matcher.traces()[0].action == MatchActionV1::Rested);
    }

    // 3) Post-only crossing synthetic open -> REJECTED, no Fill.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s3", makeOrder(3, "BTCUSDT", Side::Buy, 0.001, 50000.0, TimeInForce::Gtc, true));
        MockDeterministicMatchingFillV1 matcher(life);
        matcher.processBar(bar("BTCUSDT", 30, 49900.0, 50100.0, 49800.0, 50000.0, 1.0));
        assert(matcher.emittedEvents().size() == 1U);
        const auto& u = updateAt(matcher.emittedEvents(), 0);
        assert(u.status == OrderLifecycleStatus::Rejected);
        assert(u.terminal_reason.classification == ErrorClass::PostOnlyWouldCross);
        assert(matcher.traces()[0].action == MatchActionV1::RejectedPostOnly);
    }

    // 4) IOC no touch -> CANCELED.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s4", makeOrder(4, "BTCUSDT", Side::Buy, 0.001, 49000.0, TimeInForce::Ioc));
        MockDeterministicMatchingFillV1 matcher(life);
        matcher.processBar(bar("BTCUSDT", 30, 50000.0, 50100.0, 49500.0, 49900.0, 1.0));
        assert(matcher.emittedEvents().size() == 1U);
        assert(updateAt(matcher.emittedEvents(), 0).status == OrderLifecycleStatus::Canceled);
        assert(matcher.traces()[0].action == MatchActionV1::IocCanceledNoFill);
    }

    // 5) Partial GTC due shared participation, then complete next bar.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s5", makeOrder(5, "BTCUSDT", Side::Buy, 0.001, 50000.0));
        MockDeterministicMatchingFillV1 matcher(life);
        // 0.005 base volume * 10% = 0.0005 synthetic executable quantity.
        matcher.processBar(bar("BTCUSDT", 30, 49900.0, 50100.0, 49800.0, 50000.0, 0.005));
        assert(matcher.emittedEvents().size() == 2U);
        assert(near(fillAt(matcher.emittedEvents(), 0).quantity, 0.0005));
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::PartiallyFilled);
        assert(near(life.findOrder(5)->remaining_quantity, 0.0005));

        matcher.clearOutput();
        matcher.processBar(bar("BTCUSDT", 31, 49900.0, 50100.0, 49800.0, 50000.0, 0.005));
        assert(matcher.emittedEvents().size() == 2U);
        assert(fillAt(matcher.emittedEvents(), 0).native_references.native_fill_id == "mock-fill-5-2");
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::Filled);
    }

    // 6) IOC partial -> Fill, PARTIALLY_FILLED, CANCELED remainder.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s6", makeOrder(6, "BTCUSDT", Side::Buy, 0.001, 50000.0, TimeInForce::Ioc));
        MockDeterministicMatchingFillV1 matcher(life);
        matcher.processBar(bar("BTCUSDT", 30, 49900.0, 50100.0, 49800.0, 50000.0, 0.005));
        assert(matcher.emittedEvents().size() == 3U);
        assert(std::holds_alternative<Fill>(matcher.emittedEvents()[0]));
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::PartiallyFilled);
        assert(updateAt(matcher.emittedEvents(), 2).status == OrderLifecycleStatus::Canceled);
        assert(life.findOrder(6)->status == OrderLifecycleStatus::Canceled);
        assert(matcher.traces().size() == 2U);
        assert(matcher.traces()[1].action == MatchActionV1::IocCanceledRemainder);
    }

    // 7) Latency is event-count based, not wall-clock based.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s7", makeOrder(7, "ETHUSDT", Side::Buy, 0.01, 3000.0));
        MatchingFillConfigV1 config;
        config.latency_events = 1U;
        MockDeterministicMatchingFillV1 matcher(life, config);
        matcher.processBar(bar("ETHUSDT", 30, 2990.0, 3010.0, 2980.0, 3005.0, 10.0));
        assert(matcher.emittedEvents().empty());
        assert(life.findOrder(7)->status == OrderLifecycleStatus::Accepted);
        assert(matcher.traces()[0].action == MatchActionV1::DeferredLatency);

        matcher.clearOutput();
        matcher.processBar(bar("ETHUSDT", 31, 2990.0, 3010.0, 2980.0, 3005.0, 10.0));
        assert(matcher.emittedEvents().size() == 2U);
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::Filled);
    }

    // 8) Shared bar liquidity allocation is deterministic by ascending order id.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "s8a", makeOrder(80, "SOLUSDT", Side::Buy, 0.6, 150.0));
        submit(life, "s8b", makeOrder(81, "SOLUSDT", Side::Buy, 0.6, 150.0));
        MockDeterministicMatchingFillV1 matcher(life);
        // volume 10 * 10% => total executable 1.0 SOL. id80 gets .6, id81 gets .4.
        matcher.processBar(bar("SOLUSDT", 30, 149.0, 151.0, 148.0, 150.0, 10.0));
        assert(matcher.emittedEvents().size() == 4U);
        assert(fillAt(matcher.emittedEvents(), 0).local_order_id == 80U);
        assert(near(fillAt(matcher.emittedEvents(), 0).quantity, 0.6));
        assert(updateAt(matcher.emittedEvents(), 1).status == OrderLifecycleStatus::Filled);
        assert(fillAt(matcher.emittedEvents(), 2).local_order_id == 81U);
        assert(near(fillAt(matcher.emittedEvents(), 2).quantity, 0.4));
        assert(updateAt(matcher.emittedEvents(), 3).status == OrderLifecycleStatus::PartiallyFilled);
    }

    // 9) Same seed/state/input gives identical synthetic price/id/quantity.
    {
        MockOrderAdmissionLifecycleV1 lifeA;
        MockOrderAdmissionLifecycleV1 lifeB;
        submit(lifeA, "det-a", makeOrder(90, "BTCUSDT", Side::Buy, 0.001, 50000.0));
        submit(lifeB, "det-b", makeOrder(90, "BTCUSDT", Side::Buy, 0.001, 50000.0));
        MatchingFillConfigV1 config;
        config.deterministic_seed = 999U;
        MockDeterministicMatchingFillV1 a(lifeA, config);
        MockDeterministicMatchingFillV1 b(lifeB, config);
        const auto obs = bar("BTCUSDT", 77, 49900.0, 50100.0, 49800.0, 50000.0, 1.0);
        a.processBar(obs);
        b.processBar(obs);
        const auto& fa = fillAt(a.emittedEvents(), 0);
        const auto& fb = fillAt(b.emittedEvents(), 0);
        assert(fa.native_references.native_fill_id == fb.native_references.native_fill_id);
        assert(fa.price == fb.price);
        assert(fa.quantity == fb.quantity);
        assert(a.traces()[0].slippage_ppm_applied == b.traces()[0].slippage_ppm_applied);
        assert(a.traces()[0].diagnostic_fee_quote == b.traces()[0].diagnostic_fee_quote);
    }

    return 0;
}
