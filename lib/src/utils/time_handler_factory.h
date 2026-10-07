#pragma once

#include "time_handler.h"


namespace TimeHandlerFactory {

// Canonical environment names shared by every business service.
inline constexpr const char* SPEED_ENV = "ALGOTRADING_TIME_SPEED";
inline constexpr const char* REAL_REFERENCE_ENV = "ALGOTRADING_TIME_REAL_REFERENCE_UTC";
inline constexpr const char* SIMULATED_REFERENCE_ENV = "ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC";

// Accepted timestamp format for both references:
//
// YYYY-MM-DDTHH:MM:SSZ
//
// UTC is explicit and no local-time interpretation is performed.
TimeHandlerConfig loadConfigFromEnvironment();

// Construct a validated TimeHandler from canonical configuration.
TimeHandler create(const TimeHandlerConfig& config);

// Convenience entry point for services that read time configuration from the environment.
TimeHandler createFromEnvironment();

} // namespace TimeHandlerFactory
