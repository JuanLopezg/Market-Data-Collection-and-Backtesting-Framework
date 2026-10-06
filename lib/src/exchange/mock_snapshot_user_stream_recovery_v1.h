#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "mock_account_margin_positions_accounting_v1.h"
#include "mock_deterministic_matching_fill_v1.h"
#include "mock_order_admission_lifecycle_v1.h"
#include "mock_recovery_codec_v1.h"

/**************************************************************************************
 * Purpose : Provide durable MOCK recovery from checkpoints, incremental events, snapshots
 *           and user-stream backfill.
 *
 * Recovery preserves canonical event ordering and marks the route unsafe when conflicting
 * identity or ordering evidence prevents deterministic reconstruction.
 *
 * Keep the public recovery contract here; persistence/replay implementation lives in the
 * matching .cpp so this header remains usable as a map of the component.
 **************************************************************************************/

namespace MockVenueV1 {

enum class RecoverySourceResultV1 {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    OutOfOrderRejected,
    IdentityConflictUnsafe
};

struct UserStreamEnvelopeV1 {
    std::uint64_t sequence = 0U;
    Event event;
};

struct UserStreamPageV1 {
    std::uint64_t requested_after_sequence = 0U;
    std::uint64_t next_cursor = 0U;
    bool has_more = false;
    std::vector<UserStreamEnvelopeV1> events;
};

struct UserStateSnapshotV1 {
    std::uint64_t snapshot_sequence = 0U;
    AccountSnapshot account;
    OpenOrdersSnapshot open_orders;
    VenueContracts::V1::FillBatch fills;
    std::string economic_fingerprint;
    std::string stream_fingerprint;
    bool recovery_safe = true;
};

class MockSnapshotUserStreamRecoveryV1 {
public:
    static constexpr std::size_t kMaxBackfillEventsV1 = 256U;

    explicit MockSnapshotUserStreamRecoveryV1(
        std::filesystem::path durable_directory,
        MatchingFillConfigV1 matching_config = {},
        MockAccountingConfigV1 accounting_config = {});

    MockOrderAdmissionLifecycleV1& lifecycle() { return lifecycle_; }
    const MockOrderAdmissionLifecycleV1& lifecycle() const { return lifecycle_; }

    MockDeterministicMatchingFillV1& matcher() { return matcher_; }
    const MockDeterministicMatchingFillV1& matcher() const { return matcher_; }

    MockAccountMarginPositionsAccountingV1& account() { return account_; }
    const MockAccountMarginPositionsAccountingV1& account() const { return account_; }

    bool recoverySafe() const
    {
        return recovery_safe_ && account_.accountingSafe();
    }

    std::uint64_t lastSequence() const
    {
        return stream_.empty() ? 0U : stream_.back().sequence;
    }

    Timestamp lastBusinessEventTime() const
    {
        return last_business_event_time_;
    }

    std::uint64_t lastCheckpointSequence() const
    {
        return checkpoint_sequence_;
    }

    const std::vector<UserStreamEnvelopeV1>& stream() const
    {
        return stream_;
    }

    RecoverySourceResultV1 submit(const SubmitOrderBatch& batch);

    RecoverySourceResultV1 cancel(const CancelOrderBatch& batch);

    RecoverySourceResultV1 modify(const ModifyOrderBatch& batch);

    RecoverySourceResultV1 processBar(const MarketBarObservationV1& observation);

    RecoverySourceResultV1 markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price);

    RecoverySourceResultV1 setLeverage(
        const std::string& canonical_asset,
        Timestamp event_time,
        unsigned int leverage);

    RecoverySourceResultV1 postRebate(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& positive_amount,
        const std::string& event_id);

    RecoverySourceResultV1 postFundingPayment(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& signed_amount,
        const std::string& event_id);

    UserStateSnapshotV1 stateSnapshot(
        Timestamp timestamp,
        std::size_t max_recent_fills = 1024U) const;

    UserStreamPageV1 streamAfter(
        std::uint64_t after_sequence,
        std::size_t max_events) const;

    VenueContracts::V1::FillBatch fillsAfter(
        std::uint64_t after_sequence,
        std::size_t max_fills,
        Timestamp produced_at) const;

    bool checkpoint();

    std::string streamFingerprint() const;

private:
    enum class SourceKind : std::uint8_t {
        Submit = 1,
        Cancel = 2,
        Modify = 3,
        Bar = 4,
        Mark = 5,
        Leverage = 6,
        Rebate = 7,
        Funding = 8
    };

    std::filesystem::path durable_directory_;
    std::filesystem::path checkpoint_path_;
    std::filesystem::path journal_path_;

    MockOrderAdmissionLifecycleV1 lifecycle_;
    MockDeterministicMatchingFillV1 matcher_;
    MockAccountMarginPositionsAccountingV1 account_;

    std::vector<UserStreamEnvelopeV1> stream_;
    std::vector<std::vector<std::uint8_t>> source_records_;

    std::map<std::string, std::string> command_fingerprints_;
    std::map<std::string, std::string> non_command_fingerprints_;

    Timestamp last_business_event_time_ = 0;
    std::uint64_t checkpoint_sequence_ = 0U;
    bool recovery_safe_ = true;
    bool replaying_ = false;

    static const char* kBoundStep52Fingerprint();

    void wireHandlers();

    void recover();

    bool persistSource(
        const std::vector<std::uint8_t>& payload);

    bool allowCommandTime(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    void noteCommand(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    RecoverySourceResultV1 checkNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    void noteNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    RecoverySourceResultV1 postExternalAccounting(
        SourceKind kind,
        AccountingEventType type,
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& amount,
        const std::string& event_id);

    void replaySource(
        const std::vector<std::uint8_t>& payload);

    std::vector<std::uint8_t> encodeSubmit(
        const SubmitOrderBatch& batch) const;

    std::vector<std::uint8_t> encodeCancel(
        const CancelOrderBatch& batch) const;

    std::vector<std::uint8_t> encodeModify(
        const ModifyOrderBatch& batch) const;

    std::vector<std::uint8_t> encodeBar(
        const MarketBarObservationV1& observation) const;

    std::vector<std::uint8_t> encodeMark(
        const std::string& asset,
        Timestamp event_time,
        double mark) const;

    std::vector<std::uint8_t> encodeLeverage(
        const std::string& asset,
        Timestamp event_time,
        unsigned int leverage) const;

    std::vector<std::uint8_t> encodeExternalAccounting(
        SourceKind kind,
        const std::string& asset,
        Timestamp event_time,
        const std::string& amount,
        const std::string& event_id) const;

    void capture(const Event& event);

    static void mixU64(
        std::uint64_t& hash,
        std::uint64_t value);

    static std::string hex64(std::uint64_t value);

    static std::string eventSummary(
        const Event& event);
};

} // namespace MockVenueV1
