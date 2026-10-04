#pragma once

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include "mock_decimal_grid_v1.h"

namespace MockVenueV1 {

inline constexpr std::size_t kMockMoneyScaleV1 = 8U;
inline constexpr std::uint64_t kMockMoneyFactorV1 = 100000000ULL;

inline bool checkedAddU64(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t* out)
{
    if (out == nullptr || a > std::numeric_limits<std::uint64_t>::max() - b)
        return false;
    *out = a + b;
    return true;
}

inline bool checkedMulU64(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t* out)
{
    if (out == nullptr)
        return false;
    if (a != 0U && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    *out = a * b;
    return true;
}

/*
 * Computes round(a*b/denominator) without requiring a*b to fit into uint64.
 * The decomposition keeps the only remainder product below denominator^2.
 */
inline bool mulDivRoundNearestU64(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t denominator,
    std::uint64_t* out)
{
    if (out == nullptr || denominator == 0U)
        return false;

    const std::uint64_t qa = a / denominator;
    const std::uint64_t ra = a % denominator;
    const std::uint64_t qb = b / denominator;
    const std::uint64_t rb = b % denominator;

    std::uint64_t q_product = 0U;
    std::uint64_t term1 = 0U;
    std::uint64_t term2 = 0U;
    std::uint64_t term3 = 0U;
    if (!checkedMulU64(qa, qb, &q_product) ||
        !checkedMulU64(q_product, denominator, &term1) ||
        !checkedMulU64(qa, rb, &term2) ||
        !checkedMulU64(qb, ra, &term3))
        return false;

    // ra, rb < denominator. In this project denominator is at most 1e8 or 1e6,
    // so the remainder product is safely bounded well below uint64 max.
    std::uint64_t remainder_product = 0U;
    if (!checkedMulU64(ra, rb, &remainder_product))
        return false;

    std::uint64_t result = 0U;
    if (!checkedAddU64(term1, term2, &result) ||
        !checkedAddU64(result, term3, &result) ||
        !checkedAddU64(
            result,
            remainder_product / denominator,
            &result))
        return false;

    const std::uint64_t remainder = remainder_product % denominator;
    if (remainder >= (denominator + 1U) / 2U) {
        if (result == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++result;
    }

    *out = result;
    return true;
}

inline bool parseSignedDecimalToScale(
    const std::string& input,
    std::size_t scale,
    std::int64_t* out)
{
    if (out == nullptr || input.empty())
        return false;

    bool negative = false;
    std::string magnitude = input;
    if (magnitude.front() == '-') {
        negative = true;
        magnitude.erase(magnitude.begin());
    } else if (magnitude.front() == '+') {
        magnitude.erase(magnitude.begin());
    }

    std::uint64_t units = 0U;
    if (magnitude.empty() ||
        !parseUnsignedDecimalToScale(magnitude, scale, &units))
        return false;

    const std::uint64_t max_positive =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    if (!negative) {
        if (units > max_positive)
            return false;
        *out = static_cast<std::int64_t>(units);
        return true;
    }

    const std::uint64_t max_negative_magnitude = max_positive + 1U;
    if (units > max_negative_magnitude)
        return false;
    if (units == max_negative_magnitude) {
        *out = std::numeric_limits<std::int64_t>::min();
        return true;
    }

    *out = -static_cast<std::int64_t>(units);
    return true;
}

inline std::string formatSignedScaled(
    std::int64_t units,
    std::size_t scale)
{
    const bool negative = units < 0;
    std::uint64_t magnitude = 0U;

    if (negative) {
        magnitude =
            units == std::numeric_limits<std::int64_t>::min()
            ? static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) + 1U
            : static_cast<std::uint64_t>(-units);
    } else {
        magnitude = static_cast<std::uint64_t>(units);
    }

    std::uint64_t factor = 1U;
    for (std::size_t i = 0; i < scale; ++i) {
        if (factor > std::numeric_limits<std::uint64_t>::max() / 10U)
            return {};
        factor *= 10U;
    }

    const std::uint64_t whole = magnitude / factor;
    const std::uint64_t fraction = magnitude % factor;

    std::ostringstream out;
    out.imbue(std::locale::classic());
    if (negative)
        out << '-';
    out << whole;
    if (scale != 0U)
        out << '.' << std::setw(static_cast<int>(scale))
            << std::setfill('0') << fraction;
    return out.str();
}

inline double signedScaledToDouble(
    std::int64_t units,
    std::size_t scale)
{
    long double factor = 1.0L;
    for (std::size_t i = 0; i < scale; ++i)
        factor *= 10.0L;
    return static_cast<double>(
        static_cast<long double>(units) / factor);
}

inline bool moneyNotionalUnits(
    std::uint64_t price_units,
    std::uint64_t quantity_units,
    std::uint64_t* out_money_units)
{
    return mulDivRoundNearestU64(
        price_units,
        quantity_units,
        kMockMoneyFactorV1,
        out_money_units);
}

inline bool ppmAmountUnits(
    std::uint64_t money_units,
    std::uint32_t ppm,
    std::uint64_t* out_units)
{
    return mulDivRoundNearestU64(
        money_units,
        static_cast<std::uint64_t>(ppm),
        1000000ULL,
        out_units);
}

inline bool signedPriceQuantityPnlUnits(
    std::int64_t price_difference_units,
    std::uint64_t quantity_units,
    std::int64_t* out_money_units)
{
    if (out_money_units == nullptr)
        return false;

    const bool negative = price_difference_units < 0;
    std::uint64_t magnitude = 0U;
    if (negative) {
        magnitude =
            price_difference_units == std::numeric_limits<std::int64_t>::min()
            ? static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) + 1U
            : static_cast<std::uint64_t>(-price_difference_units);
    } else {
        magnitude = static_cast<std::uint64_t>(price_difference_units);
    }

    std::uint64_t result = 0U;
    if (!mulDivRoundNearestU64(
            magnitude,
            quantity_units,
            kMockMoneyFactorV1,
            &result) ||
        result > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        return false;

    *out_money_units = negative
        ? -static_cast<std::int64_t>(result)
        : static_cast<std::int64_t>(result);
    return true;
}

inline bool weightedAveragePriceUnits(
    std::uint64_t old_average_units,
    std::uint64_t old_quantity_units,
    std::uint64_t new_price_units,
    std::uint64_t added_quantity_units,
    std::uint64_t* out_average_units)
{
    if (out_average_units == nullptr)
        return false;

    const std::uint64_t total_quantity =
        old_quantity_units + added_quantity_units;
    if (total_quantity < old_quantity_units || total_quantity == 0U)
        return false;

    if (old_quantity_units == 0U) {
        *out_average_units = new_price_units;
        return true;
    }

    const bool new_above = new_price_units >= old_average_units;
    const std::uint64_t difference = new_above
        ? new_price_units - old_average_units
        : old_average_units - new_price_units;

    std::uint64_t adjustment = 0U;
    if (!mulDivRoundNearestU64(
            difference,
            added_quantity_units,
            total_quantity,
            &adjustment))
        return false;

    if (new_above) {
        if (old_average_units >
            std::numeric_limits<std::uint64_t>::max() - adjustment)
            return false;
        *out_average_units = old_average_units + adjustment;
    } else {
        if (adjustment > old_average_units)
            return false;
        *out_average_units = old_average_units - adjustment;
    }
    return true;
}

} // namespace MockVenueV1
