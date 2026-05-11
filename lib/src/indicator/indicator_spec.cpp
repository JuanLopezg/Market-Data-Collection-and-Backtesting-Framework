#include "indicator_spec.h"

#include <functional>
#include <sstream>


namespace {

template <typename T>
void hashCombine(std::size_t& seed, const T& value)
{
    seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

} // namespace


std::size_t IndicatorSpecHash::operator()(const IndicatorSpec& spec) const
{
    std::size_t seed = 0;

    hashCombine(seed, static_cast<int>(spec.kind));
    hashCombine(seed, static_cast<int>(spec.source));
    hashCombine(seed, spec.length);
    hashCombine(seed, spec.offset);

    return seed;
}


std::string indicatorKindToString(IndicatorKind kind)
{
    switch (kind) {
        case IndicatorKind::SMA:
            return "SMA";
        case IndicatorKind::EMA:
            return "EMA";
        case IndicatorKind::RSI:
            return "RSI";
        case IndicatorKind::ATR:
            return "ATR";
        case IndicatorKind::ROC:
            return "ROC";
        case IndicatorKind::Highest:
            return "Highest";
        case IndicatorKind::Lowest:
            return "Lowest";
        default:
            return "UnknownIndicator";
    }
}


std::string priceFieldToString(PriceField field)
{
    switch (field) {
        case PriceField::Open:
            return "Open";
        case PriceField::High:
            return "High";
        case PriceField::Low:
            return "Low";
        case PriceField::Close:
            return "Close";
        case PriceField::Volume:
            return "Volume";
        default:
            return "UnknownField";
    }
}


std::string indicatorSpecToString(const IndicatorSpec& spec)
{
    std::ostringstream oss;

    oss << indicatorKindToString(spec.kind)
        << "("
        << priceFieldToString(spec.source)
        << ", "
        << spec.length
        << ")";

    if (spec.offset > 0) {
        oss << "[" << spec.offset << "]";
    }

    return oss.str();
}