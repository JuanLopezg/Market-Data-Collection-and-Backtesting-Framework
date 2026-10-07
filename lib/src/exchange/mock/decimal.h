#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <iomanip>
#include <sstream>

// Exact decimal conversion and checked fixed-point arithmetic for order grids, cash and PnL.

namespace MockVenue {

// Convert decimal text into an integer number of 10^-target_scale units.
// Extra fractional digits are accepted only when all discarded digits are zero;
// prices and quantities are never silently rounded onto the venue grid.
inline bool parseUnsignedDecimalToScale(
    const std::string& input,
    std::size_t target_scale,
    std::uint64_t* out_units)
{
    if (out_units == nullptr || input.empty())
        return false;

    std::size_t pos = 0;
    if (input[pos] == '+')
        ++pos;
    if (pos == input.size() || input[pos] == '-')
        return false;

    std::string digits;
    bool saw_digit = false;
    bool saw_dot = false;
    std::size_t fractional_digits = 0;

    while (pos < input.size() && input[pos] != 'e' && input[pos] != 'E') {
        const char ch = input[pos++];
        if (ch == '.') {
            if (saw_dot)
                return false;
            saw_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9')
            return false;
        digits.push_back(ch);
        saw_digit = true;
        if (saw_dot)
            ++fractional_digits;
    }
    if (!saw_digit)
        return false;

    int exponent10 = 0;
    if (pos < input.size()) {
        ++pos;
        if (pos == input.size())
            return false;

        bool negative_exp = false;
        if (input[pos] == '+' || input[pos] == '-') {
            negative_exp = input[pos] == '-';
            ++pos;
        }
        if (pos == input.size())
            return false;

        int parsed = 0;
        for (; pos < input.size(); ++pos) {
            const char ch = input[pos];
            if (ch < '0' || ch > '9')
                return false;
            if (parsed > 100000)
                return false;
            parsed = parsed * 10 + (ch - '0');
        }
        exponent10 = negative_exp ? -parsed : parsed;
    }

    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        *out_units = 0;
        return true;
    }
    digits.erase(0, first_nonzero);

    const long long shift =
        static_cast<long long>(target_scale) -
        static_cast<long long>(fractional_digits) +
        static_cast<long long>(exponent10);

    if (shift < 0) { // Reducing precision must discard only zero digits.
        const std::size_t remove = static_cast<std::size_t>(-shift);
        if (remove > digits.size())
            return false;
        for (std::size_t i = digits.size() - remove; i < digits.size(); ++i) {
            if (digits[i] != '0')
                return false;
        }
        digits.resize(digits.size() - remove);
        if (digits.empty()) {
            *out_units = 0;
            return true;
        }
    } else {
        const auto append = static_cast<std::size_t>(shift);
        if (append > 20U)
            return false;
        digits.append(append, '0');
    }

    std::uint64_t value = 0;
    for (const char ch : digits) {
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }

    *out_units = value;
    return true;
}

inline bool doubleToScaledUnitsExact(
    double value,
    std::size_t target_scale,
    std::uint64_t* out_units)
{
    if (!std::isfinite(value) || value < 0.0 || out_units == nullptr)
        return false;

    char buffer[128];
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec != std::errc())
        return false;

    return parseUnsignedDecimalToScale(
        std::string(buffer, result.ptr), target_scale, out_units);
}

inline bool divisibleByIncrement(
    double value,
    const std::string& increment,
    std::size_t scale,
    std::uint64_t* out_value_units = nullptr)
{
    std::uint64_t value_units = 0;
    std::uint64_t increment_units = 0;

    if (!doubleToScaledUnitsExact(value, scale, &value_units) ||
        !parseUnsignedDecimalToScale(increment, scale, &increment_units) ||
        increment_units == 0U)
        return false;

    if (out_value_units != nullptr)
        *out_value_units = value_units;

    return value_units % increment_units == 0U;
}

inline bool meetsMinimum(
    std::uint64_t value_units,
    const std::string& minimum,
    std::size_t scale)
{
    std::uint64_t minimum_units = 0;
    return parseUnsignedDecimalToScale(minimum, scale, &minimum_units) &&
           value_units >= minimum_units;
}

inline bool meetsMinimumNotional(
    std::uint64_t price_units,
    std::size_t price_scale,
    std::uint64_t size_units,
    std::size_t size_scale,
    const std::string& minimum_notional)
{
    if (price_units == 0U || size_units == 0U)
        return false;

    std::uint64_t threshold = 0;
    if (!parseUnsignedDecimalToScale(
            minimum_notional, price_scale + size_scale, &threshold))
        return false;

    // Compare price*size against the threshold using ceiling division, avoiding
    // a potentially overflowing intermediate price*size product.
    const std::uint64_t required_price =
        threshold / size_units + (threshold % size_units == 0U ? 0U : 1U);

    return price_units >= required_price;
}

} // namespace MockVenue

namespace MockVenue {

inline constexpr std::size_t kMockMoneyScale = 8U; // Cash/PnL precision: one unit is 0.00000001 settlement currency.
inline constexpr std::uint64_t kMockMoneyFactor = 100000000ULL;

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

    const std::uint64_t qa = a / denominator; // Decompose both factors into quotient and remainder.
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

// For the current MOCK rules, price and quantity each use eight decimal places.
// Dividing their product by 10^8 converts it back to eight-decimal money units.
inline bool moneyNotionalUnits(
    std::uint64_t price_units,
    std::uint64_t quantity_units,
    std::uint64_t* out_money_units)
{
    return mulDivRoundNearestU64(
        price_units,
        quantity_units,
        kMockMoneyFactor,
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
            kMockMoneyFactor,
            &result) ||
        result > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        return false;

    *out_money_units = negative
        ? -static_cast<std::int64_t>(result)
        : static_cast<std::int64_t>(result);
    return true;
}

// Update the old average by a weighted price difference rather than summing
// large price*quantity products. Round the adjustment to the nearest price unit.
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

} // namespace MockVenue
