#pragma once

#include <stdexcept>
#include <string>
#include <string_view>


/**************************************************************************************
 * Type    : RuntimeMode
 * Purpose : Bootstrap-only runtime mode. Business code should consume Clock through the
 *           common interface and must not branch on LIVE/TESTNET/REPLAY semantics.
 **************************************************************************************/
enum class RuntimeMode {
    Live,
    Testnet,
    Replay
};

inline RuntimeMode parseRuntimeMode(std::string_view value)
{
    if (value == "live" || value == "LIVE")
        return RuntimeMode::Live;
    if (value == "testnet" || value == "TESTNET")
        return RuntimeMode::Testnet;
    if (value == "replay" || value == "REPLAY")
        return RuntimeMode::Replay;
    throw std::invalid_argument("--runtime-mode must be live, testnet or replay");
}

inline const char* runtimeModeName(RuntimeMode mode)
{
    switch (mode) {
    case RuntimeMode::Live:
        return "LIVE";
    case RuntimeMode::Testnet:
        return "TESTNET";
    case RuntimeMode::Replay:
        return "REPLAY";
    }
    return "UNKNOWN";
}
