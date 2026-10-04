#pragma once

#include <string>
#include <utility>

/**************************************************************************************
 * Header  : venue_errors.h
 * Step    : 47A — Canonical Multi-Exchange Architecture Baseline
 * Purpose : Canonical error/reject vocabulary while preserving native venue evidence
 *
 * retryable is explicit evidence supplied by the concrete adapter/policy. The core must
 * not infer retryability from native strings. Unknown or unsupported semantics remain
 * fail-closed at safety/routing boundaries.
 **************************************************************************************/
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
