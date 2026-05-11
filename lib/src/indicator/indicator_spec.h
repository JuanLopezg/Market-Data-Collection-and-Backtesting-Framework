#pragma once

#include <cstddef>
#include <string>


/**************************************************************************************
 * Type    : IndicatorKind
 * Purpose : Identifies the type of indicator requested
 **************************************************************************************/
enum class IndicatorKind {
    SMA,
    EMA,
    RSI,
    ATR,
    ROC,
    Highest,
    Lowest
};


/**************************************************************************************
 * Type    : PriceField
 * Purpose : Identifies which OHLCV field an indicator should use as input
 **************************************************************************************/
enum class PriceField {
    Open,
    High,
    Low,
    Close,
    Volume
};


/**************************************************************************************
 * Type    : IndicatorSpec
 * Purpose : Fully describes one parameterized indicator request
 *
 * Examples:
 *   RSI(Close, 14)        -> kind = RSI, source = Close, length = 14
 *   SMA(Volume, 25)       -> kind = SMA, source = Volume, length = 25
 *   ROC(Close, 5)         -> kind = ROC, source = Close, length = 5
 *   Highest(High, 20)     -> kind = Highest, source = High, length = 20
 *
 * offset:
 *   offset = 0 means current value.
 *   offset = 1 means previous bar's indicator value.
 **************************************************************************************/
struct IndicatorSpec {
    IndicatorKind kind;
    PriceField source;
    unsigned int length;
    unsigned int offset = 0;

    bool operator==(const IndicatorSpec& other) const {
        return kind == other.kind &&
               source == other.source &&
               length == other.length &&
               offset == other.offset;
    }
};


/**************************************************************************************
 * Type    : IndicatorSpecHash
 * Purpose : Allows IndicatorSpec to be used as a key in std::unordered_map
 **************************************************************************************/
struct IndicatorSpecHash {
    std::size_t operator()(const IndicatorSpec& spec) const;
};


std::string indicatorKindToString(IndicatorKind kind);
std::string priceFieldToString(PriceField field);
std::string indicatorSpecToString(const IndicatorSpec& spec);