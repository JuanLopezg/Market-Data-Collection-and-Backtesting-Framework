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

#include "account.h"
#include "matching.h"
#include "orders.h"
#include "recovery_codec.h"

// Provide durable MOCK recovery from checkpoints, incremental events, snapshots
// and user-stream backfill.
//
// Recovery preserves canonical event ordering and marks the route unsafe when conflicting
// identity or ordering evidence prevents deterministic reconstruction.
//
// Keep the public recovery contract here; persistence/replay implementation lives in the
// matching .cpp so this header remains usable as a map of the component.

namespace MockVenue {

enum class RecoverySourceResult {
    Applied = 0,
    DuplicateIgnored,
    InvalidRejected,
    OutOfOrderRejected,
    IdentityConflictUnsafe
};

// Sequence numbers order all venue events, including events sharing a timestamp.
// They are recovery cursors and must not be confused with business time.
struct UserStreamEnvelope {
    std::uint64_t sequence = 0U;
    Event event;
};

struct UserStreamPage {
    std::uint64_t requested_after_sequence = 0U;
    std::uint64_t next_cursor = 0U;
    bool has_more = false;
    std::vector<UserStreamEnvelope> events;
};

struct UserStateSnapshot {
    std::uint64_t snapshot_sequence = 0U;
    AccountSnapshot account;
    OpenOrdersSnapshot open_orders;
    VenueContracts::V1::FillBatch fills;
    std::string economic_fingerprint;
    std::string stream_fingerprint;
    bool recovery_safe = true;
};

class MockRecovery {
public:
    static constexpr std::size_t kMaxBackfillEvents = 256U;

    explicit MockRecovery(
        std::filesystem::path durable_directory,
        MatchingFillConfig matching_config = {},
        MockAccountingConfig accounting_config = {});

    MockOrders& lifecycle() { return lifecycle_; }
    const MockOrders& lifecycle() const { return lifecycle_; }

    MockMatching& matcher() { return matcher_; }
    const MockMatching& matcher() const { return matcher_; }

    MockAccount& account() { return account_; }
    const MockAccount& account() const { return account_; }

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

    const std::vector<UserStreamEnvelope>& stream() const
    {
        return stream_;
    }

    RecoverySourceResult submit(const SubmitOrderBatch& batch);

    RecoverySourceResult cancel(const CancelOrderBatch& batch);

    RecoverySourceResult modify(const ModifyOrderBatch& batch);

    RecoverySourceResult processBar(const MarketBarObservation& observation);

    RecoverySourceResult markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price);

    RecoverySourceResult setLeverage(
        const std::string& canonical_asset,
        Timestamp event_time,
        unsigned int leverage);

    RecoverySourceResult postRebate(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& positive_amount,
        const std::string& event_id);

    RecoverySourceResult postFundingPayment(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& signed_amount,
        const std::string& event_id);

    UserStateSnapshot stateSnapshot(
        Timestamp timestamp,
        std::size_t max_recent_fills = 1024U) const;

    UserStreamPage streamAfter(
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

    MockOrders lifecycle_;
    MockMatching matcher_;
    MockAccount account_;

    std::vector<UserStreamEnvelope> stream_;
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

    RecoverySourceResult checkNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    void noteNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time);

    RecoverySourceResult postExternalAccounting(
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
        const MarketBarObservation& observation) const;

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

} // namespace MockVenue
