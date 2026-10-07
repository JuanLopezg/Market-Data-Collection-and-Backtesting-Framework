#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <variant>

#include <unistd.h>

#include "mock/reconciliation.h"
#include "mock/hash.h"

using namespace VenueContracts::V1;
using namespace MockVenue;

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
    double quantity,
    double price)
{
    const auto* entry =
        findByCanonicalAssetExact(asset);
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
    o.client_order_id =
        "step54-" + std::to_string(id);
    return o;
}

SubmitOrderBatch submitOne(
    const std::string& request_id,
    const LimitOrderIntent& order,
    Timestamp ts)
{
    SubmitOrderBatch batch;
    batch.request = req(request_id, ts);
    batch.items.push_back(
        SubmitOrderItem{
            "item-" + request_id,
            order
        });
    return batch;
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
    MarketBarObservation observation;
    observation.canonical_asset = asset;
    observation.event_time = ts;
    observation.bar.open = open;
    observation.bar.high = high;
    observation.bar.low = low;
    observation.bar.close = close;
    observation.bar.volume = volume;
    assert(observation.valid());
    return observation;
}

bool hasIssue(
    const ReconciliationReport& report,
    ReconciliationIssueKind kind)
{
    return std::any_of(
        report.issues.begin(),
        report.issues.end(),
        [&](const ReconciliationIssue& issue) {
            return issue.kind == kind;
        });
}

} // namespace

int main()
{
    assert(
        Sha256::hexDigest("abc") ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("algoTrading-step54-" +
         std::to_string(::getpid()));

    std::error_code ignored;
    std::filesystem::remove_all(
        dir, ignored);

    std::string clean_ledger_head;
    std::string clean_economic_fingerprint;
    std::uint64_t checkpoint_sequence = 0U;

    {
        MockRecovery runtime(dir);
        MockReconciliation recon;

        assert(runtime.setLeverage(
            "BTCUSDT", 90, 2U) ==
            RecoverySourceResult::Applied);

        assert(runtime.submit(
            submitOne(
                "submit-btc",
                makeOrder(
                    1001,
                    "BTCUSDT",
                    0.20,
                    100.0),
                100)) ==
            RecoverySourceResult::Applied);

        // 10% of 1.0 volume => 0.1 fill.
        assert(runtime.processBar(
            bar(
                "BTCUSDT",
                110,
                101.0,
                102.0,
                99.0,
                100.0,
                1.0)) ==
            RecoverySourceResult::Applied);

        // Second bar completes the remaining 0.1.
        assert(runtime.processBar(
            bar(
                "BTCUSDT",
                120,
                101.0,
                103.0,
                98.0,
                101.0,
                1.0)) ==
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

        // Keep one explicit open order for open-order reconciliation.
        assert(runtime.submit(
            submitOne(
                "submit-eth-resting",
                makeOrder(
                    2001,
                    "ETHUSDT",
                    0.10,
                    100.0),
                130)) ==
            RecoverySourceResult::Applied);

        // Buy limit 100 is not touched by an ETH bar around 3000 -> RESTING.
        assert(runtime.processBar(
            bar(
                "ETHUSDT",
                140,
                3000.0,
                3010.0,
                2990.0,
                3005.0,
                10.0)) ==
            RecoverySourceResult::Applied);

        const ReconciliationReport clean =
            recon.reconcile(runtime, 140);

        assert(clean.state ==
               ReconciliationState::Clean);
        assert(clean.clean());
        assert(clean.issues.empty());
        assert(clean.local_sequence ==
               runtime.lastSequence());
        assert(clean.venue_sequence ==
               runtime.lastSequence());
        assert(clean.ledger_head_hash.size() == 64U);
        assert(recon.canRouteNewOrders(runtime));

        const auto local =
            recon.buildLocalExpected(
                runtime.stream());

        assert(local.complete);
        assert(local.ledger.valid);
        assert(local.ledger.fills.size() == 2U);
        assert(local.ledger.entries.size() == 6U);
        assert(local.open_orders.size() == 1U);
        assert(local.open_orders.begin()->first == 2001U);

        const auto venue =
            runtime.stateSnapshot(
                140,
                std::numeric_limits<std::size_t>::max());

        assert(venue.fills.fills.size() == 2U);
        assert(venue.open_orders.orders.size() == 1U);
        assert(venue.account.positions.size() == 1U);

        // Independent ledger cash and position projection match venue truth.
        const auto btc_position =
            local.ledger.positions.find("BTCUSDT");
        assert(btc_position !=
               local.ledger.positions.end());
        assert(btc_position->second.signed_quantity_units ==
               20000000LL);

        // PENDING is freshness/missing evidence, not a fake CLEAN/BLOCKED verdict.
        {
            auto stale = venue;
            assert(stale.snapshot_sequence > 0U);
            --stale.snapshot_sequence;
            const auto pending =
                recon.compare(local, stale);
            assert(pending.state ==
                   ReconciliationState::Pending);
            assert(hasIssue(
                pending,
                ReconciliationIssueKind::SequenceMismatch));
        }

        // Fresh cash contradiction => BLOCKED.
        {
            auto mismatch = venue;
            assert(!mismatch.account.balances.empty());
            mismatch.account.balances[0].total += 1.0;

            const auto blocked =
                recon.compare(local, mismatch);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::CashMismatch));
        }

        // Fresh position contradiction => BLOCKED.
        {
            auto mismatch = venue;
            assert(!mismatch.account.positions.empty());
            mismatch.account.positions[0].signed_quantity += 0.01;

            const auto blocked =
                recon.compare(local, mismatch);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::PositionQuantityMismatch));
        }

        // Fresh open-order contradiction => BLOCKED.
        {
            auto mismatch = venue;
            mismatch.open_orders.orders.clear();

            const auto blocked =
                recon.compare(local, mismatch);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::OpenOrderMissingOnVenue));
        }

        // Fresh fill contradiction => BLOCKED.
        {
            auto mismatch = venue;
            assert(!mismatch.fills.fills.empty());
            mismatch.fills.fills.erase(
                mismatch.fills.fills.begin());

            const auto blocked =
                recon.compare(local, mismatch);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::FillMissingOnVenue));
        }

        // Ledger tamper is detected independently of venue snapshot values.
        {
            auto tampered = local;
            assert(!tampered.ledger.entries.empty());
            tampered.ledger.entries[0].cash_delta_units += 1;

            const auto blocked =
                recon.compare(tampered, venue);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::LedgerIntegrityMismatch));
        }

        // Recovery unsafe is a hard block.
        {
            auto unsafe = venue;
            unsafe.recovery_safe = false;
            const auto blocked =
                recon.compare(local, unsafe);
            assert(blocked.state ==
                   ReconciliationState::Blocked);
            assert(hasIssue(
                blocked,
                ReconciliationIssueKind::RecoveryUnsafe));
        }

        // CLEAN gate permits exactly the new submit; state advances immediately make
        // reconciliation stale, so a second new order cannot route without re-check.
        const std::uint64_t before_submit =
            runtime.lastSequence();

        assert(recon.submitIfSafe(
            runtime,
            submitOne(
                "submit-sol",
                makeOrder(
                    3001,
                    "SOLUSDT",
                    0.10,
                    150.0),
                150)) ==
            NewOrderRouteResult::Submitted);

        assert(runtime.lastSequence() >
               before_submit);
        assert(!recon.canRouteNewOrders(runtime));

        const std::uint64_t before_blocked =
            runtime.lastSequence();

        assert(recon.submitIfSafe(
            runtime,
            submitOne(
                "submit-xrp-blocked",
                makeOrder(
                    3002,
                    "XRPUSDT",
                    10.0,
                    1.0),
                151)) ==
            NewOrderRouteResult::BlockedByReconciliation);

        assert(runtime.lastSequence() ==
               before_blocked);

        // Reconcile new state, then routing may proceed again.
        const auto clean_after_submit =
            recon.reconcile(runtime, 150);
        assert(clean_after_submit.clean());
        assert(recon.canRouteNewOrders(runtime));

        clean_ledger_head =
            clean_after_submit.ledger_head_hash;
        clean_economic_fingerprint =
            runtime.account().economicFingerprint();

        assert(runtime.checkpoint());
        checkpoint_sequence =
            runtime.lastCheckpointSequence();
        assert(checkpoint_sequence ==
               runtime.lastSequence());
    }

    // Restart: Step53 recovery reconstructs the source stream. Step54 must reconcile
    // CLEAN again with the same economic fingerprint and ledger head.
    {
        MockRecovery recovered(dir);
        MockReconciliation recon;

        assert(recovered.recoverySafe());
        assert(recovered.lastSequence() ==
               checkpoint_sequence);
        assert(recovered.account().economicFingerprint() ==
               clean_economic_fingerprint);

        const auto report =
            recon.reconcile(
                recovered,
                recovered.lastBusinessEventTime());

        assert(report.clean());
        assert(report.ledger_head_hash ==
               clean_ledger_head);
        assert(recon.canRouteNewOrders(recovered));
    }

    std::filesystem::remove_all(
        dir, ignored);
    return 0;
}
