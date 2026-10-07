#pragma once

#include <string>
#include <utility>

// Shared venue vocabulary and the versioned adapter contract. The V1 namespace records the contract format.

// Canonical error/reject vocabulary while preserving native venue evidence
//
// retryable is explicit evidence supplied by the concrete adapter/policy. The core must
// not infer retryability from native strings. Unknown or unsupported semantics remain
// fail-closed at safety/routing boundaries.
namespace VenueContracts {

enum class VenueErrorClass {
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
    VenueUnavailable,
    Unknown
};

struct VenueError {
    VenueErrorClass classification = VenueErrorClass::Unknown;
    bool retryable = false;

    // Adapter-owned evidence. These remain opaque to venue-neutral Strategy/Risk/Planner.
    std::string native_code;
    std::string native_reason;
    std::string detail;
};

inline const char* toString(VenueErrorClass value)
{
    switch (value) {
    case VenueErrorClass::None: return "NONE";
    case VenueErrorClass::TransportUnavailable: return "TRANSPORT_UNAVAILABLE";
    case VenueErrorClass::AuthenticationFailed: return "AUTHENTICATION_FAILED";
    case VenueErrorClass::AuthorizationFailed: return "AUTHORIZATION_FAILED";
    case VenueErrorClass::RateLimited: return "RATE_LIMITED";
    case VenueErrorClass::InvalidRequest: return "INVALID_REQUEST";
    case VenueErrorClass::UnsupportedCapability: return "UNSUPPORTED_CAPABILITY";
    case VenueErrorClass::UnknownAsset: return "UNKNOWN_ASSET";
    case VenueErrorClass::InvalidPriceIncrement: return "INVALID_PRICE_INCREMENT";
    case VenueErrorClass::InvalidQuantityIncrement: return "INVALID_QUANTITY_INCREMENT";
    case VenueErrorClass::BelowMinimum: return "BELOW_MINIMUM";
    case VenueErrorClass::InsufficientFundsOrMargin: return "INSUFFICIENT_FUNDS_OR_MARGIN";
    case VenueErrorClass::ReduceOnlyViolation: return "REDUCE_ONLY_VIOLATION";
    case VenueErrorClass::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
    case VenueErrorClass::NoLiquidity: return "NO_LIQUIDITY";
    case VenueErrorClass::OrderNotFound: return "ORDER_NOT_FOUND";
    case VenueErrorClass::DuplicateRequest: return "DUPLICATE_REQUEST";
    case VenueErrorClass::VenueUnavailable: return "VENUE_UNAVAILABLE";
    case VenueErrorClass::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace VenueContracts

// Stable v1 adapter error/reject taxonomy plus opaque native evidence
//
// Local PortfolioRisk rejections do not belong to this taxonomy. Unknown safety-relevant
// venue outcomes remain fail-closed and require reconciliation rather than blind retry.
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
