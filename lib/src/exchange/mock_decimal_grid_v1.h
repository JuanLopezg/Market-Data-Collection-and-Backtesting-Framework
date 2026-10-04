#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace MockVenueV1 {

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

    if (shift < 0) {
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

    const std::uint64_t required_price =
        threshold / size_units + (threshold % size_units == 0U ? 0U : 1U);

    return price_units >= required_price;
}

} // namespace MockVenueV1
