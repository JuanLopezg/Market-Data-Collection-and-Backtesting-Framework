#include "manual_requests.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <iomanip>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* HEADER = "MANUAL_REQUEST_V1";
constexpr std::size_t MAX_TARGETS = 100;
constexpr std::uintmax_t MAX_FILE_BYTES = 64 * 1024;

// Readable by the dashboard through the shared group, never by others.
constexpr auto RECORD_PERMISSIONS = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;

std::string oneLine(std::string value)
{
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

std::string utcNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

// Write a whole file durably: temporary file, fsync, rename.
void writeFileAtomically(const fs::path& target, const std::string& content)
{
    const fs::path temporary = target.parent_path() / ("." + target.filename().string() + ".tmp");
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (descriptor < 0)
        throw std::runtime_error("Cannot create " + temporary.string());
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t result = ::write(descriptor, content.data() + written, content.size() - written);
        if (result <= 0) {
            ::close(descriptor);
            fs::remove(temporary);
            throw std::runtime_error("Cannot write " + temporary.string());
        }
        written += static_cast<std::size_t>(result);
    }
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
        fs::remove(temporary);
        throw std::runtime_error("Cannot sync " + temporary.string());
    }
    fs::permissions(temporary, RECORD_PERMISSIONS, fs::perm_options::replace);
    fs::rename(temporary, target);
}

std::string readSmallFile(const fs::path& file)
{
    if (fs::file_size(file) > MAX_FILE_BYTES)
        throw std::invalid_argument("request file is larger than 64 KB");
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open " + file.string());
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

} // namespace


ManualRequestFolder::ManualRequestFolder(fs::path root)
    : root_(std::move(root)),
      requests_(root_ / "requests"),
      processed_(root_ / "processed"),
      resync_state_(root_ / "resync_state")
{
    if (!fs::is_directory(requests_) || !fs::is_directory(processed_))
        throw std::invalid_argument("Manual request folder needs requests/ and processed/: " + root_.string());
}

std::optional<fs::path> ManualRequestFolder::nextRequest() const
{
    std::optional<fs::path> oldest;
    for (const auto& entry : fs::directory_iterator(requests_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".request")
            continue;
        // The name starts with the queue time in nanoseconds: name order is arrival order.
        if (!oldest || entry.path().filename() < oldest->filename())
            oldest = entry.path();
    }
    return oldest;
}

ManualRequest ManualRequestFolder::read(const fs::path& file) const
{
    static const std::regex idPattern("^manual-[0-9a-f]{8,64}$");
    static const std::regex assetPattern("^[A-Z0-9]{2,30}$");

    ManualRequest request;
    request.file = file;
    std::istringstream lines(readSmallFile(file));
    std::string line;
    bool header = true;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        request.raw_lines.push_back(line);
        if (header) {
            if (line != HEADER)
                throw std::invalid_argument("first line is not " + std::string(HEADER));
            header = false;
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
            throw std::invalid_argument("line without '=': " + line);
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "request_id")
            request.request_id = value;
        else if (key == "actor")
            request.actor = value;
        else if (key == "request_hash")
            request.request_hash = value;
        else if (key == "created_at")
            request.created_at = value;
        else if (key == "target") {
            const std::size_t comma = value.find(',');
            if (comma == std::string::npos)
                throw std::invalid_argument("target without weight: " + value);
            const std::string asset = value.substr(0, comma);
            std::size_t parsed = 0;
            const std::string weightText = value.substr(comma + 1);
            const double weight = std::stod(weightText, &parsed);
            if (!std::regex_match(asset, assetPattern))
                throw std::invalid_argument("invalid asset name: " + asset);
            if (parsed != weightText.size() || !std::isfinite(weight) || weight < 0.0)
                throw std::invalid_argument("invalid weight for " + asset);
            if (!request.weights.emplace(asset, weight).second)
                throw std::invalid_argument("asset appears twice: " + asset);
        }
        else
            throw std::invalid_argument("unknown field: " + key);
    }
    if (header)
        throw std::invalid_argument("empty request file");
    if (!std::regex_match(request.request_id, idPattern))
        throw std::invalid_argument("invalid request_id");
    if (request.actor.empty() || request.request_hash.empty() || request.created_at.empty())
        throw std::invalid_argument("actor, request_hash and created_at are required");
    if (request.weights.size() > MAX_TARGETS)
        throw std::invalid_argument("more than 100 targets");
    return request;
}

bool ManualRequestFolder::alreadyProcessed(const fs::path& file) const
{
    return fs::exists(processed_ / file.filename());
}

void ManualRequestFolder::finish(
    const fs::path& file,
    const std::vector<std::string>& requestLines,
    const ManualOutcome& outcome) const
{
    std::ostringstream record;
    for (const std::string& line : requestLines)
        record << line << '\n';
    if (requestLines.empty())
        record << HEADER << '\n';
    record << "result=" << (outcome.executed ? "EXECUTED" : "REJECTED") << '\n';
    record << "detail=" << oneLine(outcome.detail) << '\n';
    record << "processed_at=" << utcNow() << '\n';
    if (outcome.execution_day != 0)
        record << "execution_day=" << outcome.execution_day << '\n';
    if (outcome.last_order != 0)
        record << "orders=" << outcome.first_order << '-' << outcome.last_order << '\n';

    writeFileAtomically(processed_ / file.filename(), record.str());
    remove(file);
}

void ManualRequestFolder::remove(const fs::path& file) const
{
    std::error_code ignored;
    fs::remove(file, ignored);
}

void ManualRequestFolder::markResyncPending() const
{
    writeFileAtomically(resync_state_, "pending\n");
}

bool ManualRequestFolder::resyncForDay(Timestamp day) const
{
    if (!fs::exists(resync_state_))
        return false;
    std::string state = readSmallFile(resync_state_);
    while (!state.empty() && (state.back() == '\n' || state.back() == '\r'))
        state.pop_back();
    if (state == "pending") {
        writeFileAtomically(resync_state_, "applied=" + std::to_string(day) + "\n");
        return true;
    }
    return state == "applied=" + std::to_string(day);
}


ManualRiskConfig loadManualRiskConfig(const std::string& path, const std::vector<StrategyID>& strategies)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open portfolio config: " + path);
    const nlohmann::json root = nlohmann::json::parse(input);

    ManualRiskConfig config;
    bool first = true;
    for (const auto& strategy : root.at("strategies")) {
        const auto id = static_cast<StrategyID>(strategy.at("id").get<unsigned long>());
        const double allocation = strategy.at("allocation_weight").get<double>();
        const double gross = strategy.at("risk").at("max_gross_leverage").get<double>();
        const double asset = strategy.at("risk").at("max_asset_weight").get<double>();
        if (!std::isfinite(allocation) || allocation < 0.0 || !std::isfinite(gross) || gross <= 0.0 ||
            !std::isfinite(asset) || asset <= 0.0)
            throw std::invalid_argument("Invalid allocation or risk limits in " + path);
        config.allocation[id] = allocation;
        config.max_gross_leverage = first ? gross : std::min(config.max_gross_leverage, gross);
        config.max_asset_weight = first ? asset : std::min(config.max_asset_weight, asset);
        first = false;
    }
    for (const StrategyID strategy : strategies)
        if (!config.allocation.contains(strategy))
            throw std::invalid_argument("Portfolio config has no strategy " + std::to_string(strategy));
    return config;
}


bool inDailyCycleWindow(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;
    const auto sinceMidnight = duration_cast<minutes>(now - floor<days>(now));
    return sinceMidnight >= hours(23) + minutes(45) || sinceMidnight < minutes(15);
}

std::optional<std::chrono::system_clock::time_point> parseUtcSeconds(const std::string& text)
{
    std::tm utc{};
    std::istringstream input(text);
    input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    if (input.fail())
        return std::nullopt;
    return std::chrono::system_clock::from_time_t(timegm(&utc));
}
