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
        MockAccountingConfigV1 accounting_config = {})
        : durable_directory_(std::move(durable_directory)),
          checkpoint_path_(durable_directory_ / "checkpoint_v1.bin"),
          journal_path_(durable_directory_ / "incremental_v1.bin"),
          lifecycle_(),
          matcher_(lifecycle_, matching_config),
          account_(std::move(accounting_config))
    {
        wireHandlers();
        recover();
    }

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

    RecoverySourceResultV1 submit(const SubmitOrderBatch& batch)
    {
        if (!batch.request.valid())
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeSubmit(batch);
        const std::string key = "CMD:" + batch.request.request_id;
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        if (!allowCommandTime(key, fp, batch.request.requested_at))
            return RecoverySourceResultV1::OutOfOrderRejected;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK submit source record");

        lifecycle_.submit(batch);
        noteCommand(key, fp, batch.request.requested_at);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 cancel(const CancelOrderBatch& batch)
    {
        if (!batch.request.valid())
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeCancel(batch);
        const std::string key = "CMD:" + batch.request.request_id;
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        if (!allowCommandTime(key, fp, batch.request.requested_at))
            return RecoverySourceResultV1::OutOfOrderRejected;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK cancel source record");

        lifecycle_.cancel(batch);
        noteCommand(key, fp, batch.request.requested_at);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 modify(const ModifyOrderBatch& batch)
    {
        if (!batch.request.valid())
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeModify(batch);
        const std::string key = "CMD:" + batch.request.request_id;
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        if (!allowCommandTime(key, fp, batch.request.requested_at))
            return RecoverySourceResultV1::OutOfOrderRejected;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK modify source record");

        lifecycle_.modify(batch);
        noteCommand(key, fp, batch.request.requested_at);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 processBar(const MarketBarObservationV1& observation)
    {
        if (!observation.valid() ||
            findByCanonicalAssetExact(observation.canonical_asset) == nullptr)
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeBar(observation);
        const std::string key =
            "BAR:" + observation.canonical_asset + ":" +
            std::to_string(observation.event_time);
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        const auto dedup = checkNonCommandIdentity(
            key, fp, observation.event_time);
        if (dedup != RecoverySourceResultV1::Applied)
            return dedup;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK bar source record");

        matcher_.processBar(observation);
        noteNonCommandIdentity(key, fp, observation.event_time);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 markToMarket(
        const std::string& canonical_asset,
        Timestamp event_time,
        double mark_price)
    {
        const CatalogEntry* entry =
            findByCanonicalAssetExact(canonical_asset);
        const RuleProfile* rules =
            entry == nullptr ? nullptr : rulesForExact(*entry);

        std::uint64_t ignored = 0U;
        if (entry == nullptr ||
            rules == nullptr ||
            !divisibleByIncrement(
                mark_price,
                rules->price_increment,
                rules->price_scale,
                &ignored))
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeMark(
            canonical_asset, event_time, mark_price);
        const std::string key =
            "MARK:" + canonical_asset + ":" +
            std::to_string(event_time);
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        const auto dedup = checkNonCommandIdentity(
            key, fp, event_time);
        if (dedup != RecoverySourceResultV1::Applied)
            return dedup;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK mark source record");

        if (!account_.markToMarket(
                canonical_asset, event_time, mark_price))
            throw std::runtime_error("durably recorded MOCK mark was rejected");

        noteNonCommandIdentity(key, fp, event_time);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 setLeverage(
        const std::string& canonical_asset,
        Timestamp event_time,
        unsigned int leverage)
    {
        const CatalogEntry* entry =
            findByCanonicalAssetExact(canonical_asset);
        const RuleProfile* rules =
            entry == nullptr ? nullptr : rulesForExact(*entry);

        if (entry == nullptr ||
            rules == nullptr ||
            leverage == 0U ||
            leverage > rules->max_leverage)
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeLeverage(
            canonical_asset, event_time, leverage);
        const std::string key =
            "LEV:" + canonical_asset + ":" +
            std::to_string(event_time);
        const std::string fp = RecoveryCodecV1::fnv1aHex(payload);

        const auto dedup = checkNonCommandIdentity(
            key, fp, event_time);
        if (dedup != RecoverySourceResultV1::Applied)
            return dedup;

        if (!persistSource(payload))
            throw std::runtime_error("unable to durably append MOCK leverage source record");

        if (!account_.setLeverageExact(canonical_asset, leverage))
            throw std::runtime_error("durably recorded MOCK leverage was rejected");

        noteNonCommandIdentity(key, fp, event_time);
        return RecoverySourceResultV1::Applied;
    }

    RecoverySourceResultV1 postRebate(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& positive_amount,
        const std::string& event_id)
    {
        return postExternalAccounting(
            SourceKind::Rebate,
            AccountingEventType::Rebate,
            canonical_asset,
            event_time,
            positive_amount,
            event_id);
    }

    RecoverySourceResultV1 postFundingPayment(
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& signed_amount,
        const std::string& event_id)
    {
        return postExternalAccounting(
            SourceKind::Funding,
            AccountingEventType::FundingPayment,
            canonical_asset,
            event_time,
            signed_amount,
            event_id);
    }

    UserStateSnapshotV1 stateSnapshot(
        Timestamp timestamp,
        std::size_t max_recent_fills = 1024U) const
    {
        UserStateSnapshotV1 snapshot;
        snapshot.snapshot_sequence = lastSequence();
        snapshot.account = account_.accountSnapshot(timestamp);
        snapshot.open_orders =
            account_.openOrdersSnapshot(lifecycle_, timestamp);
        snapshot.fills.venue = context();
        snapshot.fills.produced_at = timestamp;
        snapshot.fills.is_snapshot = true;
        snapshot.fills.next_cursor =
            std::to_string(snapshot.snapshot_sequence);

        std::vector<Fill> fills;
        for (const auto& envelope : stream_) {
            if (std::holds_alternative<Fill>(envelope.event))
                fills.push_back(std::get<Fill>(envelope.event));
        }

        if (fills.size() > max_recent_fills) {
            fills.erase(
                fills.begin(),
                fills.begin() +
                    static_cast<std::ptrdiff_t>(
                        fills.size() - max_recent_fills));
        }
        snapshot.fills.fills = std::move(fills);

        snapshot.economic_fingerprint =
            account_.economicFingerprint();
        snapshot.stream_fingerprint =
            streamFingerprint();
        snapshot.recovery_safe = recoverySafe();

        if (!snapshot.account.valid() ||
            !snapshot.open_orders.valid() ||
            !snapshot.fills.valid())
            throw std::runtime_error("invalid MOCK Step53 state snapshot");

        return snapshot;
    }

    UserStreamPageV1 streamAfter(
        std::uint64_t after_sequence,
        std::size_t max_events) const
    {
        UserStreamPageV1 page;
        page.requested_after_sequence = after_sequence;
        page.next_cursor = after_sequence;

        const std::size_t bounded =
            std::max<std::size_t>(
                1U,
                std::min<std::size_t>(
                    max_events,
                    kMaxBackfillEventsV1));

        for (const auto& envelope : stream_) {
            if (envelope.sequence <= after_sequence)
                continue;
            if (page.events.size() == bounded) {
                page.has_more = true;
                break;
            }
            page.events.push_back(envelope);
            page.next_cursor = envelope.sequence;
        }

        if (!page.events.empty() &&
            page.next_cursor < lastSequence())
            page.has_more = true;

        return page;
    }

    VenueContracts::V1::FillBatch fillsAfter(
        std::uint64_t after_sequence,
        std::size_t max_fills,
        Timestamp produced_at) const
    {
        VenueContracts::V1::FillBatch batch;
        batch.venue = context();
        batch.produced_at = produced_at;
        batch.is_snapshot = false;

        const std::size_t bounded =
            std::max<std::size_t>(
                1U,
                std::min<std::size_t>(
                    max_fills,
                    kMaxBackfillEventsV1));

        std::uint64_t cursor = after_sequence;
        for (const auto& envelope : stream_) {
            if (envelope.sequence <= after_sequence)
                continue;

            cursor = envelope.sequence;
            if (std::holds_alternative<Fill>(envelope.event)) {
                batch.fills.push_back(
                    std::get<Fill>(envelope.event));
                if (batch.fills.size() == bounded)
                    break;
            }
        }

        batch.next_cursor = std::to_string(cursor);
        if (!batch.valid())
            throw std::runtime_error("invalid MOCK Step53 fill backfill");
        return batch;
    }

    bool checkpoint()
    {
        if (!recoverySafe())
            return false;

        RecoveryCodecV1::Writer writer;
        writer.string("MOCK_STEP53_CHECKPOINT_V1");
        writer.string(kBoundStep52Fingerprint());
        writer.u64(lastSequence());
        writer.string(account_.economicFingerprint());
        writer.u64(
            static_cast<std::uint64_t>(
                source_records_.size()));

        for (const auto& record : source_records_)
            writer.blob(record);

        if (!RecoveryCodecV1::durableWriteAtomic(
                checkpoint_path_,
                writer.bytes()))
            return false;

        if (!RecoveryCodecV1::durableTruncate(
                journal_path_))
            return false;

        checkpoint_sequence_ = lastSequence();
        return true;
    }

    std::string streamFingerprint() const
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto& envelope : stream_) {
            mixU64(hash, envelope.sequence);
            const std::string summary =
                eventSummary(envelope.event);
            for (const unsigned char ch : summary) {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= 1099511628211ULL;
            }
        }
        return hex64(hash);
    }

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

    static const char* kBoundStep52Fingerprint()
    {
        return "4fa2a7cc0e8c43f75f7fdcab2cd5b305755c48426ac3f13b4c10ed765dfac746";
    }

    void wireHandlers()
    {
        lifecycle_.setEventHandler(
            [this](const Event& event) {
                capture(event);
            });

        matcher_.setEventHandler(
            [this](const Event& event) {
                if (std::holds_alternative<Fill>(event)) {
                    capture(event);
                    const auto result =
                        account_.consume(event);
                    if (result ==
                        FillApplyResultV1::IdentityConflictUnsafe) {
                        recovery_safe_ = false;
                        throw std::runtime_error(
                            "conflicting canonical fill during Step53");
                    }
                    if (result ==
                        FillApplyResultV1::InvalidRejected)
                        throw std::runtime_error(
                            "matcher emitted fill rejected by Step52 account");
                    return;
                }
                capture(event);
            });

        account_.setEventHandler(
            [this](const Event& event) {
                capture(event);
            });
    }

    void recover()
    {
        replaying_ = true;
        try {
            if (std::filesystem::exists(checkpoint_path_)) {
                const auto bytes =
                    RecoveryCodecV1::readWhole(checkpoint_path_);
                RecoveryCodecV1::Reader reader(bytes);

                if (reader.string() !=
                    "MOCK_STEP53_CHECKPOINT_V1")
                    throw std::runtime_error(
                        "unsupported MOCK Step53 checkpoint");

                if (reader.string() !=
                    kBoundStep52Fingerprint())
                    throw std::runtime_error(
                        "Step53 checkpoint bound to wrong Step52 fingerprint");

                const std::uint64_t expected_sequence =
                    reader.u64();
                const std::string expected_economic =
                    reader.string();
                const std::uint64_t count =
                    reader.u64();

                for (std::uint64_t i = 0; i < count; ++i) {
                    auto record = reader.blob();
                    replaySource(record);
                    source_records_.push_back(
                        std::move(record));
                }

                if (!reader.done())
                    throw std::runtime_error(
                        "extra bytes in MOCK Step53 checkpoint");

                if (lastSequence() != expected_sequence ||
                    account_.economicFingerprint() !=
                        expected_economic)
                    throw std::runtime_error(
                        "MOCK Step53 checkpoint replay divergence");

                checkpoint_sequence_ =
                    expected_sequence;
            }

            const auto journal =
                RecoveryCodecV1::readFramed(
                    journal_path_);
            for (const auto& record : journal) {
                replaySource(record);
                source_records_.push_back(record);
            }
        } catch (...) {
            replaying_ = false;
            recovery_safe_ = false;
            throw;
        }
        replaying_ = false;
    }

    bool persistSource(
        const std::vector<std::uint8_t>& payload)
    {
        if (replaying_)
            return true;

        if (!RecoveryCodecV1::durableAppendFramed(
                journal_path_,
                payload))
            return false;

        source_records_.push_back(payload);
        return true;
    }

    bool allowCommandTime(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time)
    {
        const auto known =
            command_fingerprints_.find(key);

        // A retry or conflict on an already-known request id is allowed to arrive
        // later with its original business timestamp. Step50 decides same/different payload.
        if (known != command_fingerprints_.end())
            return true;

        if (event_time < last_business_event_time_) {
            recovery_safe_ = false;
            return false;
        }

        (void)fingerprint;
        return true;
    }

    void noteCommand(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time)
    {
        command_fingerprints_.emplace(
            key, fingerprint);
        if (event_time > last_business_event_time_)
            last_business_event_time_ =
                event_time;
    }

    RecoverySourceResultV1 checkNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time)
    {
        const auto known =
            non_command_fingerprints_.find(key);

        if (known != non_command_fingerprints_.end()) {
            if (known->second == fingerprint)
                return RecoverySourceResultV1::DuplicateIgnored;
            recovery_safe_ = false;
            return RecoverySourceResultV1::IdentityConflictUnsafe;
        }

        if (event_time < last_business_event_time_) {
            recovery_safe_ = false;
            return RecoverySourceResultV1::OutOfOrderRejected;
        }

        return RecoverySourceResultV1::Applied;
    }

    void noteNonCommandIdentity(
        const std::string& key,
        const std::string& fingerprint,
        Timestamp event_time)
    {
        non_command_fingerprints_.emplace(
            key, fingerprint);
        if (event_time > last_business_event_time_)
            last_business_event_time_ =
                event_time;
    }

    RecoverySourceResultV1 postExternalAccounting(
        SourceKind kind,
        AccountingEventType type,
        const std::string& canonical_asset,
        Timestamp event_time,
        const std::string& amount,
        const std::string& event_id)
    {
        if (event_id.empty() ||
            findByCanonicalAssetExact(canonical_asset) == nullptr)
            return RecoverySourceResultV1::InvalidRejected;

        std::int64_t amount_units = 0;
        if (!parseSignedDecimalToScale(
                amount,
                kMockMoneyScaleV1,
                &amount_units) ||
            (type == AccountingEventType::Rebate &&
             amount_units <= 0))
            return RecoverySourceResultV1::InvalidRejected;

        const auto payload = encodeExternalAccounting(
            kind,
            canonical_asset,
            event_time,
            amount,
            event_id);
        const std::string key =
            "ACCT:" + event_id;
        const std::string fp =
            RecoveryCodecV1::fnv1aHex(payload);

        const auto dedup =
            checkNonCommandIdentity(
                key, fp, event_time);
        if (dedup != RecoverySourceResultV1::Applied)
            return dedup;

        if (!persistSource(payload))
            throw std::runtime_error(
                "unable to durably append MOCK accounting source record");

        ExternalAccountingApplyResultV1 result =
            type == AccountingEventType::Rebate
            ? account_.postRebate(
                canonical_asset,
                event_time,
                amount,
                event_id)
            : account_.postFundingPayment(
                canonical_asset,
                event_time,
                amount,
                event_id);

        if (result ==
            ExternalAccountingApplyResultV1::IdentityConflictUnsafe) {
            recovery_safe_ = false;
            throw std::runtime_error(
                "conflicting external accounting event after durable append");
        }
        if (result ==
            ExternalAccountingApplyResultV1::InvalidRejected)
            throw std::runtime_error(
                "durably recorded external accounting event was rejected");

        noteNonCommandIdentity(
            key, fp, event_time);
        return result ==
            ExternalAccountingApplyResultV1::DuplicateIgnored
            ? RecoverySourceResultV1::DuplicateIgnored
            : RecoverySourceResultV1::Applied;
    }

    void replaySource(
        const std::vector<std::uint8_t>& payload)
    {
        RecoveryCodecV1::Reader reader(payload);
        const SourceKind kind =
            static_cast<SourceKind>(reader.u8());

        switch (kind) {
        case SourceKind::Submit: {
            auto batch =
                RecoveryCodecV1::submitBatch(reader);
            if (!reader.done())
                throw std::runtime_error("extra submit recovery bytes");
            const std::string key =
                "CMD:" + batch.request.request_id;
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            if (!allowCommandTime(
                    key, fp, batch.request.requested_at))
                throw std::runtime_error(
                    "out-of-order submit inside durable recovery history");
            lifecycle_.submit(batch);
            noteCommand(
                key, fp, batch.request.requested_at);
            return;
        }
        case SourceKind::Cancel: {
            auto batch =
                RecoveryCodecV1::cancelBatch(reader);
            if (!reader.done())
                throw std::runtime_error("extra cancel recovery bytes");
            const std::string key =
                "CMD:" + batch.request.request_id;
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            if (!allowCommandTime(
                    key, fp, batch.request.requested_at))
                throw std::runtime_error(
                    "out-of-order cancel inside durable recovery history");
            lifecycle_.cancel(batch);
            noteCommand(
                key, fp, batch.request.requested_at);
            return;
        }
        case SourceKind::Modify: {
            auto batch =
                RecoveryCodecV1::modifyBatch(reader);
            if (!reader.done())
                throw std::runtime_error("extra modify recovery bytes");
            const std::string key =
                "CMD:" + batch.request.request_id;
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            if (!allowCommandTime(
                    key, fp, batch.request.requested_at))
                throw std::runtime_error(
                    "out-of-order modify inside durable recovery history");
            lifecycle_.modify(batch);
            noteCommand(
                key, fp, batch.request.requested_at);
            return;
        }
        case SourceKind::Bar: {
            auto observation =
                RecoveryCodecV1::marketBar(reader);
            if (!reader.done())
                throw std::runtime_error("extra bar recovery bytes");
            const std::string key =
                "BAR:" + observation.canonical_asset + ":" +
                std::to_string(observation.event_time);
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            const auto dedup =
                checkNonCommandIdentity(
                    key, fp, observation.event_time);
            if (dedup ==
                RecoverySourceResultV1::IdentityConflictUnsafe ||
                dedup ==
                RecoverySourceResultV1::OutOfOrderRejected)
                throw std::runtime_error(
                    "unsafe bar inside durable recovery history");
            if (dedup !=
                RecoverySourceResultV1::DuplicateIgnored) {
                matcher_.processBar(observation);
                noteNonCommandIdentity(
                    key, fp, observation.event_time);
            }
            return;
        }
        case SourceKind::Mark: {
            const std::string asset = reader.string();
            const Timestamp event_time =
                static_cast<Timestamp>(reader.u64());
            const double mark = reader.dbl();
            if (!reader.done())
                throw std::runtime_error("extra mark recovery bytes");

            const std::string key =
                "MARK:" + asset + ":" +
                std::to_string(event_time);
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            const auto dedup =
                checkNonCommandIdentity(
                    key, fp, event_time);
            if (dedup ==
                RecoverySourceResultV1::IdentityConflictUnsafe ||
                dedup ==
                RecoverySourceResultV1::OutOfOrderRejected)
                throw std::runtime_error(
                    "unsafe mark inside durable recovery history");
            if (dedup !=
                RecoverySourceResultV1::DuplicateIgnored) {
                if (!account_.markToMarket(
                        asset, event_time, mark))
                    throw std::runtime_error(
                        "recovered mark rejected");
                noteNonCommandIdentity(
                    key, fp, event_time);
            }
            return;
        }
        case SourceKind::Leverage: {
            const std::string asset = reader.string();
            const Timestamp event_time =
                static_cast<Timestamp>(reader.u64());
            const unsigned int leverage =
                reader.u32();
            if (!reader.done())
                throw std::runtime_error("extra leverage recovery bytes");

            const std::string key =
                "LEV:" + asset + ":" +
                std::to_string(event_time);
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            const auto dedup =
                checkNonCommandIdentity(
                    key, fp, event_time);
            if (dedup ==
                RecoverySourceResultV1::IdentityConflictUnsafe ||
                dedup ==
                RecoverySourceResultV1::OutOfOrderRejected)
                throw std::runtime_error(
                    "unsafe leverage inside durable recovery history");
            if (dedup !=
                RecoverySourceResultV1::DuplicateIgnored) {
                if (!account_.setLeverageExact(
                        asset, leverage))
                    throw std::runtime_error(
                        "recovered leverage rejected");
                noteNonCommandIdentity(
                    key, fp, event_time);
            }
            return;
        }
        case SourceKind::Rebate:
        case SourceKind::Funding: {
            const std::string asset = reader.string();
            const Timestamp event_time =
                static_cast<Timestamp>(reader.u64());
            const std::string amount = reader.string();
            const std::string event_id = reader.string();
            if (!reader.done())
                throw std::runtime_error(
                    "extra external accounting recovery bytes");

            const std::string key = "ACCT:" + event_id;
            const std::string fp =
                RecoveryCodecV1::fnv1aHex(payload);
            const auto dedup =
                checkNonCommandIdentity(
                    key, fp, event_time);
            if (dedup ==
                RecoverySourceResultV1::IdentityConflictUnsafe ||
                dedup ==
                RecoverySourceResultV1::OutOfOrderRejected)
                throw std::runtime_error(
                    "unsafe accounting event inside durable recovery history");
            if (dedup ==
                RecoverySourceResultV1::DuplicateIgnored)
                return;

            ExternalAccountingApplyResultV1 result =
                kind == SourceKind::Rebate
                ? account_.postRebate(
                    asset, event_time, amount, event_id)
                : account_.postFundingPayment(
                    asset, event_time, amount, event_id);

            if (result !=
                ExternalAccountingApplyResultV1::Applied)
                throw std::runtime_error(
                    "recovered external accounting event rejected");

            noteNonCommandIdentity(
                key, fp, event_time);
            return;
        }
        }

        throw std::runtime_error(
            "unknown MOCK Step53 recovery source kind");
    }

    std::vector<std::uint8_t> encodeSubmit(
        const SubmitOrderBatch& batch) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Submit));
        RecoveryCodecV1::submitBatch(
            writer, batch);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeCancel(
        const CancelOrderBatch& batch) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Cancel));
        RecoveryCodecV1::cancelBatch(
            writer, batch);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeModify(
        const ModifyOrderBatch& batch) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Modify));
        RecoveryCodecV1::modifyBatch(
            writer, batch);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeBar(
        const MarketBarObservationV1& observation) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Bar));
        RecoveryCodecV1::marketBar(
            writer, observation);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeMark(
        const std::string& asset,
        Timestamp event_time,
        double mark) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Mark));
        writer.string(asset);
        writer.u64(
            static_cast<std::uint64_t>(
                event_time));
        writer.dbl(mark);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeLeverage(
        const std::string& asset,
        Timestamp event_time,
        unsigned int leverage) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(
                SourceKind::Leverage));
        writer.string(asset);
        writer.u64(
            static_cast<std::uint64_t>(
                event_time));
        writer.u32(leverage);
        return writer.bytes();
    }

    std::vector<std::uint8_t> encodeExternalAccounting(
        SourceKind kind,
        const std::string& asset,
        Timestamp event_time,
        const std::string& amount,
        const std::string& event_id) const
    {
        RecoveryCodecV1::Writer writer;
        writer.u8(
            static_cast<std::uint8_t>(kind));
        writer.string(asset);
        writer.u64(
            static_cast<std::uint64_t>(
                event_time));
        writer.string(amount);
        writer.string(event_id);
        return writer.bytes();
    }

    void capture(const Event& event)
    {
        UserStreamEnvelopeV1 envelope;
        envelope.sequence =
            static_cast<std::uint64_t>(
                stream_.size()) + 1U;
        envelope.event = event;
        stream_.push_back(
            std::move(envelope));
    }

    static void mixU64(
        std::uint64_t& hash,
        std::uint64_t value)
    {
        for (unsigned i = 0; i < 8U; ++i) {
            hash ^= static_cast<unsigned char>(
                (value >> (i * 8U)) & 0xffU);
            hash *= 1099511628211ULL;
        }
    }

    static std::string hex64(std::uint64_t value)
    {
        static const char* digits =
            "0123456789abcdef";
        std::string out(16U, '0');
        for (int i = 15; i >= 0; --i) {
            out[static_cast<std::size_t>(i)] =
                digits[value & 0xfU];
            value >>= 4U;
        }
        return out;
    }

    static std::string eventSummary(
        const Event& event)
    {
        return std::visit(
            [](const auto& value) -> std::string {
                using T =
                    std::decay_t<decltype(value)>;

                if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::OperationResult>) {
                    return "OP|" +
                        value.request.request_id + "|" +
                        std::to_string(
                            static_cast<int>(value.command)) + "|" +
                        std::to_string(
                            static_cast<int>(value.scope)) + "|" +
                        (value.accepted ? "1" : "0") + "|" +
                        std::to_string(value.item_results.size());
                } else if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::OrderUpdate>) {
                    return "OU|" +
                        std::to_string(value.local_order_id) + "|" +
                        std::to_string(value.timestamp) + "|" +
                        VenueContracts::V1::toString(value.status) + "|" +
                        value.native_status;
                } else if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::Fill>) {
                    return "F|" +
                        value.native_references.native_fill_id + "|" +
                        std::to_string(value.local_order_id) + "|" +
                        std::to_string(value.timestamp) + "|" +
                        std::to_string(value.quantity) + "|" +
                        std::to_string(value.price);
                } else if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::AccountingEvent>) {
                    return "A|" +
                        value.correlation_id + "|" +
                        std::to_string(value.timestamp) + "|" +
                        VenueContracts::V1::toString(value.type) + "|" +
                        value.amount;
                } else if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::AccountSnapshot>) {
                    return "AS|" +
                        std::to_string(value.timestamp) + "|" +
                        std::to_string(value.equity);
                } else if constexpr (
                    std::is_same_v<
                        T,
                        VenueContracts::V1::OpenOrdersSnapshot>) {
                    return "OS|" +
                        std::to_string(value.timestamp) + "|" +
                        std::to_string(value.orders.size());
                } else {
                    return "FB|" +
                        std::to_string(value.produced_at) + "|" +
                        std::to_string(value.fills.size()) + "|" +
                        value.next_cursor;
                }
            },
            event);
    }
};

} // namespace MockVenueV1
