#pragma once

#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "snapshot_examples.h"
#include "state_store.h"

namespace persistence_test {

// Backends own real database lifetimes and provide reopen()/failSnapshotWrites().
// The assertions below are identical for both StateStore implementations.
template <typename Backend>
class StateStoreContract : public Backend {};

TYPED_TEST_SUITE_P(StateStoreContract);

TYPED_TEST_P(StateStoreContract, R1_FreshStoreHasNoSnapshotOrAudit)
{
    EXPECT_FALSE(this->store_->load().has_value());
    EXPECT_TRUE(this->store_->loadFills().empty());
    this->reopen();
    EXPECT_FALSE(this->store_->load().has_value());
}

TYPED_TEST_P(StateStoreContract, R1_ZeroCashFlatSnapshotSurvivesReopen)
{
    const TradingStateSnapshot flat;
    this->store_->save(flat);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, flat);
    EXPECT_TRUE(this->store_->loadFills().empty());
}

TYPED_TEST_P(StateStoreContract, R2_AllOperationalStateSurvivesRestart)
{
    const auto snapshot = exampleSnapshot();
    this->store_->save(snapshot);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    ASSERT_EQ(loaded->orders.size(), 1U);
    EXPECT_EQ(loaded->orders.front().remainingQuantity(), 3.0);
    ASSERT_EQ(loaded->pending_plans.size(), 1U);
    EXPECT_EQ(loaded->pending_plans.front().plan.get("SOL").action, RebalanceAction::Hold);
    EXPECT_EQ(loaded->pending_plans.front().plan.referenceCapital() *
              loaded->pending_plans.front().plan.get("BTC").target_weight, 250.0);
    EXPECT_TRUE(this->store_->loadFills().empty()); // Intent is not a fill.
}

TYPED_TEST_P(StateStoreContract, R2_RestartPreservesFullDoublePrecision)
{
    auto snapshot = exampleSnapshot();
    snapshot.account_cash = 1234.5678901234567;
    snapshot.account_positions["BTC"] = 0.12345678901234566;
    snapshot.strategies.front().virtual_positions["BTC"] = 0.12345678901234566;
    auto fill = exampleFill();
    fill.quantity = 0.12345678901234566;
    fill.price = 75.12345678901234;
    fill.commission = 0.00012345678901234567;
    this->store_->save(snapshot, fill);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    const auto fills = this->store_->loadFills();
    ASSERT_EQ(fills.size(), 1U);
    expectFill(fills.front(), fill);
}

TYPED_TEST_P(StateStoreContract, R3_ReplacementRemovesStaleOperationalChildren)
{
    this->store_->save(exampleSnapshot(), exampleFill());
    TradingStateSnapshot flat;
    flat.account_cash = 1048.0;
    flat.last_bar_close_timestamp = 20260103;
    flat.last_execution_timestamp = 20260103;
    flat.next_order_id = 43;
    this->store_->save(flat);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, flat);
    const auto fills = this->store_->loadFills();
    ASSERT_EQ(fills.size(), 1U);
    expectFill(fills.front(), exampleFill());
}

TYPED_TEST_P(StateStoreContract, R4_DuplicateFillAndCheckpointKeepOneAuditRow)
{
    auto snapshot = exampleSnapshot();
    snapshot.account_positions.erase("ETH");
    snapshot.strategies.pop_back();
    const auto fill = exampleFill();
    this->store_->save(snapshot, fill);
    this->store_->save(snapshot, fill);
    this->store_->save(snapshot);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    EXPECT_EQ(loaded->account_cash, 849.0); // 1000 - 2*75 - 1.
    EXPECT_EQ(loaded->account_positions.at("BTC"), 2.0);
    const auto fills = this->store_->loadFills();
    ASSERT_EQ(fills.size(), 1U);
    expectFill(fills.front(), fill);
}

TYPED_TEST_P(StateStoreContract, R4_DistinctPartialFillIdsRemainDistinct)
{
    auto snapshot = exampleSnapshot();
    snapshot.account_positions.erase("ETH");
    snapshot.strategies.pop_back();
    this->store_->save(snapshot, exampleFill());
    auto second = exampleFill(102);
    second.quantity = 1.0;
    second.commission = 0.5;
    snapshot.account_cash = 773.5; // 849 - 1*75 - 0.5.
    snapshot.account_positions["BTC"] = 3.0;
    snapshot.strategies.front().virtual_positions["BTC"] = 3.0;
    snapshot.orders.front().filled_quantity = 3.0;
    snapshot.processed_fill_ids.push_back(102);
    this->store_->save(snapshot, second);
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    auto fills = this->store_->loadFills();
    ASSERT_EQ(fills.size(), 2U);
    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) { return a.fill_id < b.fill_id; });
    expectFill(fills[0], exampleFill());
    expectFill(fills[1], second);
}

TYPED_TEST_P(StateStoreContract, R4_FillIdsAbove32BitsDoNotCollide)
{
    auto snapshot = exampleSnapshot();
    snapshot.processed_fill_ids = {1, 4294967297ULL};
    this->store_->save(snapshot, exampleFill(1));
    this->store_->save(snapshot, exampleFill(4294967297ULL));
    this->reopen();
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, snapshot);
    auto fills = this->store_->loadFills();
    ASSERT_EQ(fills.size(), 2U);
    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) { return a.fill_id < b.fill_id; });
    expectFill(fills[0], exampleFill(1));
    expectFill(fills[1], exampleFill(4294967297ULL));
}

TYPED_TEST_P(StateStoreContract, R5_InvalidFillRollsBackBothSnapshotAndAudit)
{
    TradingStateSnapshot before;
    before.account_cash = 1000.0;
    this->store_->save(before);
    std::vector<Fill> invalid;
    const auto add = [&](auto change) {
        auto fill = exampleFill();
        change(fill);
        invalid.push_back(fill);
    };
    add([](Fill& f) { f.fill_id = 0; });
    add([](Fill& f) { f.coin.clear(); });
    for (const double value : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        add([value](Fill& f) { f.quantity = value; });
        add([value](Fill& f) { f.price = value; });
    }
    for (const double value : {-1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()})
        add([value](Fill& f) { f.commission = value; });
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_THROW(this->store_->save(exampleSnapshot(), invalid[index]), std::invalid_argument);
        const auto loaded = this->store_->load();
        ASSERT_TRUE(loaded.has_value());
        expectSnapshot(*loaded, before);
        EXPECT_TRUE(this->store_->loadFills().empty());
    }
    EXPECT_NO_THROW(this->store_->save(exampleSnapshot(), exampleFill()));
    EXPECT_EQ(this->store_->loadFills().size(), 1U);
}

TYPED_TEST_P(StateStoreContract, R5_DatabaseFailureAfterAuditInsertRollsBackBoth)
{
    TradingStateSnapshot before;
    before.account_cash = 1000.0;
    this->store_->save(before);
    this->failSnapshotWrites(true);
    EXPECT_THROW(this->store_->save(exampleSnapshot(), exampleFill()), std::runtime_error);
    const auto loaded = this->store_->load();
    ASSERT_TRUE(loaded.has_value());
    expectSnapshot(*loaded, before);
    EXPECT_TRUE(this->store_->loadFills().empty());
    this->failSnapshotWrites(false);
    this->store_->save(exampleSnapshot(), exampleFill());
    this->reopen();
    const auto committed = this->store_->load();
    ASSERT_TRUE(committed.has_value());
    expectSnapshot(*committed, exampleSnapshot());
    EXPECT_EQ(this->store_->loadFills().size(), 1U);
}

TYPED_TEST_P(StateStoreContract, R6_LoadRejectsUnknownSchema)
{
    auto snapshot = exampleSnapshot();
    snapshot.schema_version = TradingStateSnapshot::CURRENT_SCHEMA_VERSION + 1;
    this->store_->save(snapshot);
    this->reopen();
    EXPECT_THROW((void)this->store_->load(), std::exception);
}

TYPED_TEST_P(StateStoreContract, R7_UnsupportedResearchOrdersDoNotCommit)
{
    TradingStateSnapshot before;
    before.account_cash = 1000.0;
    this->store_->save(before);
    for (int field = 0; field < 3; ++field) {
        SCOPED_TRACE(field);
        auto snapshot = exampleSnapshot();
        if (field == 0) snapshot.orders.front().request.entry_stop_price = 110.0;
        if (field == 1) snapshot.orders.front().request.protective_stop_price = 120.0;
        if (field == 2) snapshot.orders.front().request.parent_order_id = 40;
        EXPECT_THROW(this->store_->save(snapshot, exampleFill()), std::invalid_argument);
        const auto loaded = this->store_->load();
        ASSERT_TRUE(loaded.has_value());
        expectSnapshot(*loaded, before);
        EXPECT_TRUE(this->store_->loadFills().empty());
    }
}

TYPED_TEST_P(StateStoreContract, R7_UnsupportedPendingTriggersDoNotCommit)
{
    TradingStateSnapshot before;
    before.account_cash = 1000.0;
    this->store_->save(before);
    for (const bool protective : {false, true}) {
        SCOPED_TRACE(protective);
        auto snapshot = exampleSnapshot();
        auto decision = RebalanceDecision::targetWeight(0.25);
        if (protective) decision.protective_stop_price = 120.0;
        else decision.entry_stop_price = 110.0;
        snapshot.pending_plans.front().plan.set("BTC", decision);
        EXPECT_THROW(this->store_->save(snapshot, exampleFill()), std::invalid_argument);
        const auto loaded = this->store_->load();
        ASSERT_TRUE(loaded.has_value());
        expectSnapshot(*loaded, before);
        EXPECT_TRUE(this->store_->loadFills().empty());
    }
}

REGISTER_TYPED_TEST_SUITE_P(StateStoreContract,
    R1_FreshStoreHasNoSnapshotOrAudit,
    R1_ZeroCashFlatSnapshotSurvivesReopen,
    R2_AllOperationalStateSurvivesRestart,
    R2_RestartPreservesFullDoublePrecision,
    R3_ReplacementRemovesStaleOperationalChildren,
    R4_DuplicateFillAndCheckpointKeepOneAuditRow,
    R4_DistinctPartialFillIdsRemainDistinct,
    R4_FillIdsAbove32BitsDoNotCollide,
    R5_InvalidFillRollsBackBothSnapshotAndAudit,
    R5_DatabaseFailureAfterAuditInsertRollsBackBoth,
    R6_LoadRejectsUnknownSchema,
    R7_UnsupportedResearchOrdersDoNotCommit,
    R7_UnsupportedPendingTriggersDoNotCommit);

} // namespace persistence_test
