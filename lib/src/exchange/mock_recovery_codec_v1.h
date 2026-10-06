#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "canonical_venue_account_v1.h"
#include "canonical_venue_orders_v1.h"
#include "mock_deterministic_matching_fill_v1.h"

/**************************************************************************************
 * Purpose : Deterministic binary serialization helpers for MOCK recovery state.
 *
 * The codec is intentionally explicit: durable bytes are version-sensitive recovery data,
 * not a general application serialization format.
 **************************************************************************************/

namespace MockVenueV1 {
namespace RecoveryCodecV1 {

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u32(std::uint32_t value)
    {
        for (unsigned i = 0; i < 4U; ++i)
            bytes_.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }

    void u64(std::uint64_t value)
    {
        for (unsigned i = 0; i < 8U; ++i)
            bytes_.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }

    void i64(std::int64_t value)
    {
        std::uint64_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void boolean(bool value) { u8(value ? 1U : 0U); }

    void dbl(double value)
    {
        static_assert(sizeof(double) == sizeof(std::uint64_t), "unexpected double size");
        std::uint64_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void string(const std::string& value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("recovery string too large");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void blob(const std::vector<std::uint8_t>& value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("recovery blob too large");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes)
        : bytes_(bytes)
    {
    }

    std::uint8_t u8()
    {
        require(1U);
        return bytes_[pos_++];
    }

    std::uint32_t u32()
    {
        require(4U);
        std::uint32_t value = 0U;
        for (unsigned i = 0; i < 4U; ++i)
            value |= static_cast<std::uint32_t>(bytes_[pos_++]) << (i * 8U);
        return value;
    }

    std::uint64_t u64()
    {
        require(8U);
        std::uint64_t value = 0U;
        for (unsigned i = 0; i < 8U; ++i)
            value |= static_cast<std::uint64_t>(bytes_[pos_++]) << (i * 8U);
        return value;
    }

    std::int64_t i64()
    {
        const std::uint64_t bits = u64();
        std::int64_t value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool boolean()
    {
        const auto value = u8();
        if (value > 1U)
            throw std::runtime_error("invalid recovery bool");
        return value != 0U;
    }

    double dbl()
    {
        const std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string string()
    {
        const auto size = static_cast<std::size_t>(u32());
        require(size);
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + pos_);
        std::string value(begin, begin + size);
        pos_ += size;
        return value;
    }

    std::vector<std::uint8_t> blob()
    {
        const auto size = static_cast<std::size_t>(u32());
        require(size);
        std::vector<std::uint8_t> value(
            bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));
        pos_ += size;
        return value;
    }

    bool done() const { return pos_ == bytes_.size(); }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t pos_ = 0U;

    void require(std::size_t count) const
    {
        if (count > bytes_.size() - pos_)
            throw std::runtime_error("truncated recovery record");
    }
};

void venueContext(
    Writer& writer,
    const VenueContracts::V1::VenueContext& value);

VenueContracts::V1::VenueContext venueContext(Reader& reader);

void marketIdentity(
    Writer& writer,
    const VenueContracts::V1::MarketIdentity& value);

VenueContracts::V1::MarketIdentity marketIdentity(Reader& reader);

void instrument(
    Writer& writer,
    const VenueContracts::V1::InstrumentIdentity& value);

VenueContracts::V1::InstrumentIdentity instrument(Reader& reader);

void nativeRefs(
    Writer& writer,
    const VenueContracts::V1::NativeReferences& value);

VenueContracts::V1::NativeReferences nativeRefs(Reader& reader);

void requestIdentity(
    Writer& writer,
    const VenueContracts::V1::RequestIdentity& value);

VenueContracts::V1::RequestIdentity requestIdentity(Reader& reader);

void limitOrder(
    Writer& writer,
    const VenueContracts::V1::LimitOrderIntent& value);

VenueContracts::V1::LimitOrderIntent limitOrder(Reader& reader);

void orderLocator(
    Writer& writer,
    const VenueContracts::V1::OrderLocator& value);

VenueContracts::V1::OrderLocator orderLocator(Reader& reader);

void submitBatch(
    Writer& writer,
    const VenueContracts::V1::SubmitOrderBatch& value);

VenueContracts::V1::SubmitOrderBatch submitBatch(Reader& reader);

void cancelBatch(
    Writer& writer,
    const VenueContracts::V1::CancelOrderBatch& value);

VenueContracts::V1::CancelOrderBatch cancelBatch(Reader& reader);

void modifyBatch(
    Writer& writer,
    const VenueContracts::V1::ModifyOrderBatch& value);

VenueContracts::V1::ModifyOrderBatch modifyBatch(Reader& reader);

void marketBar(
    Writer& writer,
    const MarketBarObservationV1& value);

MarketBarObservationV1 marketBar(Reader& reader);

bool durableWriteAtomic(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes);

bool durableAppendFramed(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& payload);

bool durableTruncate(const std::filesystem::path& path);

std::vector<std::uint8_t> readWhole(
    const std::filesystem::path& path);

std::vector<std::vector<std::uint8_t>> readFramed(
    const std::filesystem::path& path);

std::string fnv1aHex(const std::vector<std::uint8_t>& bytes);

} // namespace RecoveryCodecV1
} // namespace MockVenueV1
