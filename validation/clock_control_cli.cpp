#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "clock_control.h"
#include "contract_json_codec.h"
#include "nats_jetstream_message_bus.h"
#include "transport_subjects.h"

namespace {

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string simulation_id;
    std::string command;
    std::string speed;
    std::string message_id;
    std::uint64_t expected_revision = 0;
};

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string("Missing value for ") + name);
            return argv[++i];
        };

        if (arg == "--nats-url")
            options.nats_url = value("--nats-url");
        else if (arg == "--simulation-id")
            options.simulation_id = value("--simulation-id");
        else if (arg == "--command")
            options.command = value("--command");
        else if (arg == "--speed")
            options.speed = value("--speed");
        else if (arg == "--expected-revision")
            options.expected_revision = std::stoull(value("--expected-revision"));
        else if (arg == "--message-id")
            options.message_id = value("--message-id");
        else
            throw std::invalid_argument("Unknown clock-control option: " + arg);
    }

    if (options.simulation_id.empty())
        throw std::invalid_argument("--simulation-id is required");
    if (options.command != "pause" && options.command != "resume" &&
        options.command != "speed")
        throw std::invalid_argument("--command must be pause, resume or speed");
    if (options.command == "speed" && options.speed.empty())
        throw std::invalid_argument("--speed is required for --command speed");
    return options;
}

void setSpeed(ClockControl& command, std::string value)
{
    if (value == "max") {
        command.mode = SimulationClockMode::MaxSpeed;
        command.speed_multiplier = 0.0;
        return;
    }
    if (value == "x1" || value == "1" || value == "realtime") {
        command.mode = SimulationClockMode::Realtime;
        command.speed_multiplier = 0.0;
        return;
    }
    if (!value.empty() && value.front() == 'x')
        value.erase(value.begin());

    std::size_t consumed = 0;
    const double multiplier = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(multiplier) || multiplier <= 0.0)
        throw std::invalid_argument("Invalid speed multiplier");
    if (multiplier == 1.0) {
        command.mode = SimulationClockMode::Realtime;
        command.speed_multiplier = 0.0;
        return;
    }
    command.mode = SimulationClockMode::Multiplier;
    command.speed_multiplier = multiplier;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseOptions(argc, argv);

        ClockControl command;
        command.metadata.schema_version = 1;
        command.metadata.correlation_id = "validation-clock-control";
        command.metadata.produced_at = 0;
        command.simulation_id = options.simulation_id;
        command.expected_revision = options.expected_revision;

        if (options.command == "pause")
            command.action = ClockControlAction::Pause;
        else if (options.command == "resume")
            command.action = ClockControlAction::Resume;
        else {
            command.action = ClockControlAction::SetSpeed;
            setSpeed(command, options.speed);
        }

        if (!options.message_id.empty()) {
            command.metadata.message_id = options.message_id;
        }
        else {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            command.metadata.message_id =
                "validation-clock-control:" + options.command + ":" +
                std::to_string(static_cast<std::uint64_t>(ticks));
        }

        NatsJetStreamMessageBus bus(options.nats_url);
        bus.publish(
            TransportSubjects::CLOCK_CONTROL,
            ContractJsonCodec::encode(command),
            command.metadata.message_id
        );
        bus.flush();

        std::cout
            << "CLOCK_CONTROL_SENT command=" << options.command
            << " simulation_id=" << command.simulation_id
            << " expected_revision=" << command.expected_revision
            << " mode=" << static_cast<int>(command.mode)
            << " speed_multiplier=" << command.speed_multiplier
            << " message_id=" << command.metadata.message_id
            << '\n';
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "CLOCK_CONTROL_FAILED error=" << error.what() << '\n';
        return 1;
    }
}
