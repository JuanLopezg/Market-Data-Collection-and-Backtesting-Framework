#pragma once

#include <string>

/**************************************************************************************
 * Header  : canonical_venue_errors_v1.h
 * Step    : 48 — Canonical Multi-Venue Adapter Contract v1
 * Purpose : Stable v1 adapter error/reject taxonomy plus opaque native evidence
 *
 * Local PortfolioRisk rejections do not belong to this taxonomy. Unknown safety-relevant
 * venue outcomes remain fail-closed and require reconciliation rather than blind retry.
 **************************************************************************************/
namespace VenueContracts {
namespace V1 {

enum class ErrorClass {
    None = 0,
    TransportUnavailable,
    AuthenticationFailed,
    AuthorizationFailed,
    RateLimited,
    InvalidRequest,
    UnsupportedCapability,
    UnknownAsset,
    InvalidPriceIncrement,
    InvalidQuantityIncrement,
    BelowMinimum,
    InsufficientFundsOrMargin,
    ReduceOnlyViolation,
    PostOnlyWouldCross,
    NoLiquidity,
    OrderNotFound,
    DuplicateRequest,
    VenueLimit,
    VenueUnavailable,
    Unknown
};

struct Error {
    ErrorClass classification = ErrorClass::Unknown;
    bool retryable = false;

    std::string native_code;
    std::string native_reason;
    std::string detail;

    bool none() const
    {
        return classification == ErrorClass::None;
    }
};

inline const char* toString(ErrorClass value)
{
    switch (value) {
    case ErrorClass::None: return "NONE";
    case ErrorClass::TransportUnavailable: return "TRANSPORT_UNAVAILABLE";
    case ErrorClass::AuthenticationFailed: return "AUTHENTICATION_FAILED";
    case ErrorClass::AuthorizationFailed: return "AUTHORIZATION_FAILED";
    case ErrorClass::RateLimited: return "RATE_LIMITED";
    case ErrorClass::InvalidRequest: return "INVALID_REQUEST";
    case ErrorClass::UnsupportedCapability: return "UNSUPPORTED_CAPABILITY";
    case ErrorClass::UnknownAsset: return "UNKNOWN_ASSET";
    case ErrorClass::InvalidPriceIncrement: return "INVALID_PRICE_INCREMENT";
    case ErrorClass::InvalidQuantityIncrement: return "INVALID_QUANTITY_INCREMENT";
    case ErrorClass::BelowMinimum: return "BELOW_MINIMUM";
    case ErrorClass::InsufficientFundsOrMargin: return "INSUFFICIENT_FUNDS_OR_MARGIN";
    case ErrorClass::ReduceOnlyViolation: return "REDUCE_ONLY_VIOLATION";
    case ErrorClass::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
    case ErrorClass::NoLiquidity: return "NO_LIQUIDITY";
    case ErrorClass::OrderNotFound: return "ORDER_NOT_FOUND";
    case ErrorClass::DuplicateRequest: return "DUPLICATE_REQUEST";
    case ErrorClass::VenueLimit: return "VENUE_LIMIT";
    case ErrorClass::VenueUnavailable: return "VENUE_UNAVAILABLE";
    case ErrorClass::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace V1
} // namespace VenueContracts
