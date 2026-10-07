// Stateless deterministic fault draws keyed by seed, ordinal and operation.
// These helpers support reproducible simulation and are not security randomness.

#pragma once

#include <cstdint>
#include <string>

namespace MockVenue {

inline std::uint64_t splitmix64(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

inline std::uint64_t fnv1a64(const std::string& text)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::uint32_t deterministicPpmDraw(
    std::uint64_t seed,
    std::uint64_t ordinal,
    const std::string& operation_key)
{
    const std::uint64_t mixed =
        splitmix64(
            seed ^
            splitmix64(ordinal + 0x6a09e667f3bcc909ULL) ^
            fnv1a64(operation_key));
    return static_cast<std::uint32_t>(mixed % 1000000ULL); // Draw in [0, 1,000,000) for ppm thresholds.
}

} // namespace MockVenue
