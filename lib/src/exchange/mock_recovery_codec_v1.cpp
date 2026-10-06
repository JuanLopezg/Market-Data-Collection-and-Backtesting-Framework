#include "mock_recovery_codec_v1.h"

#include <cerrno>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace MockVenueV1 {
namespace RecoveryCodecV1 {

void venueContext(Writer& w, const VenueContracts::V1::VenueContext& value)
{
    w.string(value.venue_id);
    w.u8(static_cast<std::uint8_t>(value.environment));
    w.string(value.custom_environment);
}

VenueContracts::V1::VenueContext venueContext(Reader& r)
{
    VenueContracts::V1::VenueContext value;
    value.venue_id = r.string();
    value.environment = static_cast<VenueContracts::VenueEnvironment>(r.u8());
    value.custom_environment = r.string();
    return value;
}

void marketIdentity(Writer& w, const VenueContracts::V1::MarketIdentity& value)
{
    w.string(value.canonical_asset);
    w.u8(static_cast<std::uint8_t>(value.product_class));
    w.string(value.quote_or_settlement_asset);
    w.string(value.contract_variant);
}

VenueContracts::V1::MarketIdentity marketIdentity(Reader& r)
{
    VenueContracts::V1::MarketIdentity value;
    value.canonical_asset = r.string();
    value.product_class = static_cast<VenueContracts::V1::ProductClass>(r.u8());
    value.quote_or_settlement_asset = r.string();
    value.contract_variant = r.string();
    return value;
}

void instrument(Writer& w, const VenueContracts::V1::InstrumentIdentity& value)
{
    marketIdentity(w, value.market);
    w.string(value.venue_symbol);
    w.string(value.venue_asset_id);
}

VenueContracts::V1::InstrumentIdentity instrument(Reader& r)
{
    VenueContracts::V1::InstrumentIdentity value;
    value.market = marketIdentity(r);
    value.venue_symbol = r.string();
    value.venue_asset_id = r.string();
    return value;
}

void nativeRefs(Writer& w, const VenueContracts::V1::NativeReferences& value)
{
    w.string(value.native_order_id);
    w.string(value.native_fill_id);
    w.string(value.native_client_order_id);
}

VenueContracts::V1::NativeReferences nativeRefs(Reader& r)
{
    VenueContracts::V1::NativeReferences value;
    value.native_order_id = r.string();
    value.native_fill_id = r.string();
    value.native_client_order_id = r.string();
    return value;
}

void requestIdentity(Writer& w, const VenueContracts::V1::RequestIdentity& value)
{
    venueContext(w, value.venue);
    w.string(value.request_id);
    w.string(value.correlation_id);
    w.u64(static_cast<std::uint64_t>(value.requested_at));
}

VenueContracts::V1::RequestIdentity requestIdentity(Reader& r)
{
    VenueContracts::V1::RequestIdentity value;
    value.venue = venueContext(r);
    value.request_id = r.string();
    value.correlation_id = r.string();
    value.requested_at = static_cast<Timestamp>(r.u64());
    return value;
}

void limitOrder(Writer& w, const VenueContracts::V1::LimitOrderIntent& value)
{
    w.u64(static_cast<std::uint64_t>(value.local_order_id));
    w.u64(static_cast<std::uint64_t>(value.strategy_id));
    w.u64(static_cast<std::uint64_t>(value.created_at));
    w.u64(static_cast<std::uint64_t>(value.active_from));
    instrument(w, value.instrument);
    w.u8(static_cast<std::uint8_t>(value.side));
    w.dbl(value.quantity);
    w.dbl(value.limit_price);
    w.u8(static_cast<std::uint8_t>(value.time_in_force));
    w.boolean(value.post_only);
    w.boolean(value.reduce_only);
    w.string(value.client_order_id);
}

VenueContracts::V1::LimitOrderIntent limitOrder(Reader& r)
{
    VenueContracts::V1::LimitOrderIntent value;
    value.local_order_id = static_cast<OrderID>(r.u64());
    value.strategy_id = static_cast<StrategyID>(r.u64());
    value.created_at = static_cast<Timestamp>(r.u64());
    value.active_from = static_cast<Timestamp>(r.u64());
    value.instrument = instrument(r);
    value.side = static_cast<VenueContracts::V1::Side>(r.u8());
    value.quantity = r.dbl();
    value.limit_price = r.dbl();
    value.time_in_force = static_cast<VenueContracts::V1::TimeInForce>(r.u8());
    value.post_only = r.boolean();
    value.reduce_only = r.boolean();
    value.client_order_id = r.string();
    return value;
}

void orderLocator(Writer& w, const VenueContracts::V1::OrderLocator& value)
{
    w.u64(static_cast<std::uint64_t>(value.local_order_id));
    instrument(w, value.instrument);
    nativeRefs(w, value.native_references);
}

VenueContracts::V1::OrderLocator orderLocator(Reader& r)
{
    VenueContracts::V1::OrderLocator value;
    value.local_order_id = static_cast<OrderID>(r.u64());
    value.instrument = instrument(r);
    value.native_references = nativeRefs(r);
    return value;
}

void submitBatch(Writer& w, const VenueContracts::V1::SubmitOrderBatch& value)
{
    requestIdentity(w, value.request);
    w.u32(static_cast<std::uint32_t>(value.items.size()));
    for (const auto& item : value.items) {
        w.string(item.item_id);
        limitOrder(w, item.order);
    }
}

VenueContracts::V1::SubmitOrderBatch submitBatch(Reader& r)
{
    VenueContracts::V1::SubmitOrderBatch value;
    value.request = requestIdentity(r);
    const auto count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        VenueContracts::V1::SubmitOrderItem item;
        item.item_id = r.string();
        item.order = limitOrder(r);
        value.items.push_back(std::move(item));
    }
    return value;
}

void cancelBatch(Writer& w, const VenueContracts::V1::CancelOrderBatch& value)
{
    requestIdentity(w, value.request);
    w.u32(static_cast<std::uint32_t>(value.items.size()));
    for (const auto& item : value.items) {
        w.string(item.item_id);
        orderLocator(w, item.order);
    }
}

VenueContracts::V1::CancelOrderBatch cancelBatch(Reader& r)
{
    VenueContracts::V1::CancelOrderBatch value;
    value.request = requestIdentity(r);
    const auto count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        VenueContracts::V1::CancelOrderItem item;
        item.item_id = r.string();
        item.order = orderLocator(r);
        value.items.push_back(std::move(item));
    }
    return value;
}

void modifyBatch(Writer& w, const VenueContracts::V1::ModifyOrderBatch& value)
{
    requestIdentity(w, value.request);
    w.u32(static_cast<std::uint32_t>(value.items.size()));
    for (const auto& item : value.items) {
        w.string(item.item_id);
        orderLocator(w, item.order);
        limitOrder(w, item.replacement);
    }
}

VenueContracts::V1::ModifyOrderBatch modifyBatch(Reader& r)
{
    VenueContracts::V1::ModifyOrderBatch value;
    value.request = requestIdentity(r);
    const auto count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        VenueContracts::V1::ModifyOrderItem item;
        item.item_id = r.string();
        item.order = orderLocator(r);
        item.replacement = limitOrder(r);
        value.items.push_back(std::move(item));
    }
    return value;
}

void marketBar(Writer& w, const MarketBarObservationV1& value)
{
    w.string(value.canonical_asset);
    w.u64(static_cast<std::uint64_t>(value.event_time));
    w.dbl(value.bar.open);
    w.dbl(value.bar.high);
    w.dbl(value.bar.low);
    w.dbl(value.bar.close);
    w.dbl(value.bar.volume);
}

MarketBarObservationV1 marketBar(Reader& r)
{
    MarketBarObservationV1 value;
    value.canonical_asset = r.string();
    value.event_time = static_cast<Timestamp>(r.u64());
    value.bar.open = r.dbl();
    value.bar.high = r.dbl();
    value.bar.low = r.dbl();
    value.bar.close = r.dbl();
    value.bar.volume = r.dbl();
    return value;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size)
{
    std::size_t written = 0U;
    while (written < size) {
        const ssize_t rc = ::write(
            fd,
            data + written,
            size - written);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        written += static_cast<std::size_t>(rc);
    }
    return true;
}

bool fsyncDirectory(const std::filesystem::path& directory)
{
    const int fd = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool durableWriteAtomic(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary =
        path.string() + ".tmp";

    const int fd = ::open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (fd < 0)
        return false;

    bool ok = writeAll(fd, bytes.data(), bytes.size());
    if (ok)
        ok = ::fsync(fd) == 0;
    if (::close(fd) != 0)
        ok = false;

    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec)
        return false;

    return fsyncDirectory(path.parent_path());
}

bool durableAppendFramed(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& payload)
{
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        return false;

    std::filesystem::create_directories(path.parent_path());

    Writer frame;
    frame.u32(static_cast<std::uint32_t>(payload.size()));
    const auto prefix = frame.bytes();

    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_APPEND,
        0644);
    if (fd < 0)
        return false;

    bool ok =
        writeAll(fd, prefix.data(), prefix.size()) &&
        writeAll(fd, payload.data(), payload.size()) &&
        ::fsync(fd) == 0;

    if (::close(fd) != 0)
        ok = false;
    return ok;
}

bool durableTruncate(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (fd < 0)
        return false;
    bool ok = ::fsync(fd) == 0;
    if (::close(fd) != 0)
        ok = false;
    if (ok)
        ok = fsyncDirectory(path.parent_path());
    return ok;
}

std::vector<std::uint8_t> readWhole(
    const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(
            "unable to open recovery file: " + path.string());

    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0)
        throw std::runtime_error("unable to size recovery file");
    in.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        in.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!in)
            throw std::runtime_error("unable to read recovery file");
    }
    return bytes;
}

std::vector<std::vector<std::uint8_t>> readFramed(
    const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
        return {};

    const auto bytes = readWhole(path);
    std::vector<std::vector<std::uint8_t>> records;
    std::size_t pos = 0U;

    while (pos < bytes.size()) {
        if (bytes.size() - pos < 4U)
            throw std::runtime_error("truncated recovery journal frame header");

        std::uint32_t size = 0U;
        for (unsigned i = 0; i < 4U; ++i)
            size |= static_cast<std::uint32_t>(bytes[pos++]) << (i * 8U);

        if (static_cast<std::size_t>(size) > bytes.size() - pos)
            throw std::runtime_error("truncated recovery journal payload");

        records.emplace_back(
            bytes.begin() + static_cast<std::ptrdiff_t>(pos),
            bytes.begin() + static_cast<std::ptrdiff_t>(
                pos + static_cast<std::size_t>(size)));
        pos += static_cast<std::size_t>(size);
    }

    return records;
}

std::string fnv1aHex(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }

    static const char* digits = "0123456789abcdef";
    std::string out(16U, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xfU];
        hash >>= 4U;
    }
    return out;
}

} // namespace RecoveryCodecV1
} // namespace MockVenueV1
