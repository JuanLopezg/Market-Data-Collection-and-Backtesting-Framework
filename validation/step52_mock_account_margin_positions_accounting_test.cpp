#include <cassert>
#include <cmath>
#include <string>
#include <variant>

#include "mock_account_margin_positions_accounting_v1.h"
#include "mock_order_admission_lifecycle_v1.h"

using namespace VenueContracts::V1;
using namespace MockVenueV1;

namespace {

bool near(double a, double b, double eps = 1e-8)
{
    return std::abs(a - b) <= eps;
}

Fill makeFill(
    const std::string& asset,
    OrderID order_id,
    Side side,
    double quantity,
    double price,
    Timestamp timestamp,
    const std::string& fill_id)
{
    const auto* entry = findByCanonicalAssetExact(asset);
    assert(entry != nullptr);

    Fill fill;
    fill.venue = context();
    fill.instrument = entry->instrument();
    fill.local_order_id = order_id;
    fill.strategy_id = 7;
    fill.timestamp = timestamp;
    fill.side = side;
    fill.quantity = quantity;
    fill.price = price;
    fill.native_references.native_order_id =
        "mock-order-" + std::to_string(order_id);
    fill.native_references.native_fill_id = fill_id;
    fill.native_references.native_client_order_id =
        "cloid-" + std::to_string(order_id);
    assert(fill.valid());
    return fill;
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
    o.client_order_id = "life-" + std::to_string(id);
    return o;
}

void submit(
    MockOrderAdmissionLifecycleV1& life,
    const std::string& request_id,
    const LimitOrderIntent& order,
    Timestamp ts)
{
    SubmitOrderBatch b;
    b.request = req(request_id, ts);
    b.items.push_back(SubmitOrderItem{"item-" + request_id, order});
    life.submit(b);
    life.clearEmittedEvents();
}

} // namespace

int main()
{
    // Fixed point helpers.
    {
        std::uint64_t notional = 0U;
        assert(moneyNotionalUnits(
            100ULL * kMockMoneyFactorV1,
            10000000ULL, // 0.1
            &notional));
        assert(notional == 10ULL * kMockMoneyFactorV1);

        std::uint64_t fee = 0U;
        assert(ppmAmountUnits(notional, 400U, &fee));
        assert(fee == 400000ULL); // 0.00400000

        std::int64_t signed_amount = 0;
        assert(parseSignedDecimalToScale("-0.25000000", 8U, &signed_amount));
        assert(signed_amount == -25000000LL);
        assert(formatSignedScaled(signed_amount, 8U) == "-0.25000000");
    }

    MockAccountMarginPositionsAccountingV1 account;

    // Initial account.
    {
        const auto s = account.accountSnapshot(1);
        assert(near(s.equity, 100000.0));
        assert(near(s.margin_used, 0.0));
        assert(s.positions.empty());
        assert(s.balances.size() == 1U);
        assert(near(s.balances[0].total, 100000.0));
        assert(near(s.balances[0].available, 100000.0));
        assert(account.accountingSafe());
    }

    assert(account.setLeverageExact("BTCUSDT", 2U));
    assert(!account.setLeverageExact("BTCUSDT", 21U));
    assert(!account.setLeverageExact("UNKNOWN", 1U));

    // Buy 0.1 BTC @ 100.00. Fill is execution authority; fee is posted canonically.
    const Fill buy = makeFill(
        "BTCUSDT", 1, Side::Buy, 0.1, 100.0, 100, "fill-1");
    Event buy_event = buy;
    assert(account.consume(buy_event) == FillApplyResultV1::Applied);
    assert(account.emittedEvents().size() == 1U);
    assert(std::holds_alternative<AccountingEvent>(account.emittedEvents()[0]));

    const auto& fee1 = std::get<AccountingEvent>(account.emittedEvents()[0]);
    assert(fee1.type == AccountingEventType::TradingFee);
    assert(fee1.amount == "-0.00400000");
    assert(fee1.timestamp == 100U);
    assert(fee1.correlation_id == "fee:fill-1");

    {
        const auto s = account.accountSnapshot(100);
        assert(s.positions.size() == 1U);
        assert(near(s.positions[0].signed_quantity, 0.1));
        assert(near(s.positions[0].entry_price, 100.0));
        assert(near(s.positions[0].mark_price, 100.0));
        assert(near(s.positions[0].unrealized_pnl, 0.0));
        assert(near(s.positions[0].leverage, 2.0));
        assert(near(s.equity, 99999.996));
        assert(near(s.margin_used, 5.0));
    }

    // Same fill is idempotent in-process: no double position/fee.
    account.clearEmittedEvents();
    const std::string fp_before_duplicate = account.economicFingerprint();
    assert(account.applyFill(buy) == FillApplyResultV1::DuplicateIgnored);
    assert(account.emittedEvents().empty());
    assert(account.economicFingerprint() == fp_before_duplicate);

    // Explicit mark -> unrealized PnL and equity.
    assert(account.markToMarket("BTCUSDT", 110, 110.0));
    {
        const auto m = account.marginState(110);
        assert(near(m.settled_cash, 99999.996));
        assert(near(m.realized_pnl, 0.0));
        assert(near(m.unrealized_pnl, 1.0));
        assert(near(m.equity, 100000.996));
        assert(near(m.gross_exposure, 11.0));
        assert(near(m.margin_used, 5.5));
        assert(near(m.available_margin, 99995.496));
    }

    // Partial close: sell .04 @120 realizes +0.80 and preserves entry=100 on .06 long.
    account.clearEmittedEvents();
    const Fill partial_close = makeFill(
        "BTCUSDT", 2, Side::Sell, 0.04, 120.0, 120, "fill-2");
    assert(account.applyFill(partial_close) == FillApplyResultV1::Applied);
    {
        const auto s = account.accountSnapshot(120);
        assert(s.positions.size() == 1U);
        assert(near(s.positions[0].signed_quantity, 0.06));
        assert(near(s.positions[0].entry_price, 100.0));
        assert(near(s.positions[0].mark_price, 120.0));
        assert(near(s.positions[0].unrealized_pnl, 1.2));
        assert(near(account.marginState(120).realized_pnl, 0.8));
        assert(std::get<AccountingEvent>(
            account.emittedEvents()[0]).amount == "-0.00192000");
    }

    // Flip long .06 to short .04 at 90. Realizes -0.60; new short entry=90.
    account.clearEmittedEvents();
    const Fill flip = makeFill(
        "BTCUSDT", 3, Side::Sell, 0.10, 90.0, 130, "fill-3");
    assert(account.applyFill(flip) == FillApplyResultV1::Applied);
    {
        const auto s = account.accountSnapshot(130);
        assert(s.positions.size() == 1U);
        assert(near(s.positions[0].signed_quantity, -0.04));
        assert(near(s.positions[0].entry_price, 90.0));
        assert(near(s.positions[0].mark_price, 90.0));
        assert(near(account.marginState(130).realized_pnl, 0.2));
    }

    // Short mark lower -> positive unrealized.
    assert(account.markToMarket("BTCUSDT", 140, 80.0));
    {
        const auto m = account.marginState(140);
        assert(near(m.unrealized_pnl, 0.4));
        assert(near(m.gross_exposure, 3.2));
        assert(near(m.margin_used, 1.6));
    }

    // Rebate/funding canonical accounting.
    account.clearEmittedEvents();
    assert(account.postRebate(
        "BTCUSDT", 150, "0.50000000", "rebate-1") ==
        ExternalAccountingApplyResultV1::Applied);
    assert(account.postFundingPayment(
        "BTCUSDT", 151, "-0.25000000", "funding-1") ==
        ExternalAccountingApplyResultV1::Applied);
    assert(account.emittedEvents().size() == 2U);
    assert(std::get<AccountingEvent>(
        account.emittedEvents()[0]).type == AccountingEventType::Rebate);
    assert(std::get<AccountingEvent>(
        account.emittedEvents()[1]).type == AccountingEventType::FundingPayment);

    account.clearEmittedEvents();
    const auto funding_fp = account.economicFingerprint();
    assert(account.postFundingPayment(
        "BTCUSDT", 151, "-0.25000000", "funding-1") ==
        ExternalAccountingApplyResultV1::DuplicateIgnored);
    assert(account.emittedEvents().empty());
    assert(account.economicFingerprint() == funding_fp);

    // Order views come from lifecycle state only.
    {
        MockOrderAdmissionLifecycleV1 life;
        submit(life, "open-a", makeOrder(1001, "ETHUSDT", 0.01, 3000.0), 200);
        submit(life, "open-b", makeOrder(1002, "SOLUSDT", 0.1, 150.0), 201);

        const auto open_before = account.openOrdersSnapshot(life, 202);
        assert(open_before.orders.size() == 2U);

        const StoredOrder* stored = life.findOrder(1002);
        assert(stored != nullptr);

        CancelOrderBatch cancel;
        cancel.request = req("cancel-sol", 203);
        CancelOrderItem item;
        item.item_id = "cancel-sol-item";
        item.order.local_order_id = stored->intent.local_order_id;
        item.order.instrument = stored->intent.instrument;
        item.order.native_references = stored->native_references;
        cancel.items.push_back(item);
        life.cancel(cancel);

        const auto open_after = account.openOrdersSnapshot(life, 204);
        assert(open_after.orders.size() == 1U);
        assert(open_after.orders[0].local_order_id == 1001U);

        const auto history = account.historicalOrders(life);
        assert(history.size() == 2U);
        assert(!history[0].terminal);
        assert(history[1].terminal);
        assert(history[1].order.status == OrderLifecycleStatus::Canceled);
    }

    // Same event-time economic inputs => same fingerprint; wall/replay speed is absent.
    {
        MockAccountMarginPositionsAccountingV1 a;
        MockAccountMarginPositionsAccountingV1 b;
        assert(a.setLeverageExact("BTCUSDT", 3U));
        assert(b.setLeverageExact("BTCUSDT", 3U));

        const Fill f1 = makeFill(
            "BTCUSDT", 10, Side::Buy, 0.2, 100.0, 500, "det-fill-1");
        const Fill f2 = makeFill(
            "BTCUSDT", 11, Side::Sell, 0.05, 110.0, 501, "det-fill-2");

        assert(a.applyFill(f1) == FillApplyResultV1::Applied);
        assert(a.applyFill(f2) == FillApplyResultV1::Applied);
        assert(a.markToMarket("BTCUSDT", 502, 105.0));
        assert(a.postFundingPayment(
            "BTCUSDT", 503, "-0.12500000", "det-funding") ==
            ExternalAccountingApplyResultV1::Applied);

        // Different amount of irrelevant CPU work cannot affect economics.
        volatile std::uint64_t sink = 0U;
        for (std::uint64_t i = 0; i < 10000U; ++i)
            sink += i;
        (void)sink;

        assert(b.applyFill(f1) == FillApplyResultV1::Applied);
        assert(b.applyFill(f2) == FillApplyResultV1::Applied);
        assert(b.markToMarket("BTCUSDT", 502, 105.0));
        assert(b.postFundingPayment(
            "BTCUSDT", 503, "-0.12500000", "det-funding") ==
            ExternalAccountingApplyResultV1::Applied);

        assert(a.economicFingerprint() == b.economicFingerprint());
        assert(a.accountSnapshot(504).equity == b.accountSnapshot(504).equity);
    }

    // Conflicting fill ID is fail-closed and marks accounting unsafe.
    {
        MockAccountMarginPositionsAccountingV1 unsafe;
        Fill original = makeFill(
            "ETHUSDT", 20, Side::Buy, 0.01, 3000.0, 600, "conflict-fill");
        assert(unsafe.applyFill(original) == FillApplyResultV1::Applied);
        const std::string before = unsafe.economicFingerprint();

        Fill conflict = original;
        conflict.price = 3100.0;
        assert(unsafe.applyFill(conflict) ==
               FillApplyResultV1::IdentityConflictUnsafe);
        assert(!unsafe.accountingSafe());
        assert(unsafe.accountSnapshot(601).positions.size() == 1U);
        assert(unsafe.economicFingerprint() != before); // unsafe flag changed, economics did not duplicate
    }

    return 0;
}
