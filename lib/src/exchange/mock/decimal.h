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
 * Computes round(a*b/denominator) (half up) with an exact 128-bit product, so any
 * denominator works. Weighted average entry prices divide by a position quantity
 * (up to ~1e10+ units at 8 decimals), where a 64-bit remainder product overflowed.
 * Fails only when the rounded result does not fit into uint64.
 */
__extension__ typedef unsigned __int128 MockU128;

inline bool mulDivRoundNearestU64(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t denominator,
    std::uint64_t* out)
{
    if (out == nullptr || denominator == 0U)
        return false;

    const MockU128 product = static_cast<MockU128>(a) * b;
    MockU128 quotient = product / denominator;
    const MockU128 remainder = product % denominator;
    if (remainder >= (static_cast<MockU128>(denominator) + 1U) / 2U)
        ++quotient;

    if (quotient > std::numeric_limits<std::uint64_t>::max())
        return false;
    const std::uint64_t result = static_cast<std::uint64_t>(quotient);

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

// Grid units -> the double nearest to units / 10^scale, so doubleToScaledUnitsExact()
// reads back the same units. Up to 2^53 units both operands are exact doubles and the
// IEEE division rounds once. A long-double division cast to double rounds twice and
// left about one value in 4,000 one ULP off the grid (rejected as SIZE_INCREMENT).
// Larger values cannot be exact in a double anyway and keep the long-double path.
inline double unsignedScaledToDouble(
    std::uint64_t units,
    std::size_t scale)
{
    if (units <= (std::uint64_t{1} << 53) && scale <= 22U) {
        double factor = 1.0;
        for (std::size_t i = 0; i < scale; ++i)
            factor *= 10.0;
        return static_cast<double>(units) / factor;
    }
    long double factor = 1.0L;
    for (std::size_t i = 0; i < scale; ++i)
        factor *= 10.0L;
    return static_cast<double>(
        static_cast<long double>(units) / factor);
}

inline double signedScaledToDouble(
    std::int64_t units,
    std::size_t scale)
{
    const std::uint64_t magnitude = units < 0
        ? static_cast<std::uint64_t>(-(units + 1)) + 1U
        : static_cast<std::uint64_t>(units);
    const double value = unsignedScaledToDouble(magnitude, scale);
    return units < 0 ? -value : value;
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
