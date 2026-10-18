#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "data_types.h"

// Manual Control requests of the trading chain, exchanged as small text files with the
// dashboard (dashboard-api/internal/provider/real_manual.go writes them):
//
//   <dir>/requests/<queued ns>-<request id>.request    written by the dashboard
//   <dir>/processed/<same name>                         the request plus its outcome
//   <dir>/resync_state                                  "pending" or "applied=YYYYMMDD"
//
// Request file:
//   MANUAL_REQUEST_V1
//   request_id=manual-0123456789abcdef
//   actor=operator / OPERATOR
//   request_hash=sha256:...
//   created_at=2026-10-14T12:00:00Z
//   target=BTCUSDT,0.400000000000      (fraction of equity; the rest stays in cash)
//
// The processed copy adds result=EXECUTED|REJECTED, detail=, processed_at=,
// execution_day=YYYYMMDD and orders=FIRST-LAST (only when orders were sent).

struct ManualRequest {
    std::filesystem::path file;
    std::string request_id;
    std::string actor;
    std::string request_hash;
    std::string created_at;               // RFC 3339 UTC, as written by the dashboard
    std::map<Coin, double> weights;       // fraction of equity per asset
    std::vector<std::string> raw_lines;   // copied into the processed record
};

struct ManualOutcome {
    bool executed = false;
    std::string detail;
    Timestamp execution_day = 0;
    OrderID first_order = 0;
    OrderID last_order = 0;               // 0 when no order was sent
};

class ManualRequestFolder {
public:
    explicit ManualRequestFolder(std::filesystem::path root);

    // Oldest queued request file, or nothing.
    std::optional<std::filesystem::path> nextRequest() const;

    // Throws std::invalid_argument with a readable reason for a malformed file.
    ManualRequest read(const std::filesystem::path& file) const;

    bool alreadyProcessed(const std::filesystem::path& file) const;

    // Writes the processed record first (atomically), then removes the request, so a
    // request with a record is never executed again.
    void finish(const std::filesystem::path& file, const std::vector<std::string>& requestLines,
                const ManualOutcome& outcome) const;
    void remove(const std::filesystem::path& file) const;

    // Re-sync marker for the next daily account snapshot.
    void markResyncPending() const;
    // True when the snapshot of `day` must ask for the re-sync. A pending marker is bound
    // to the first day it is used for, so a redelivered snapshot of that day keeps it.
    bool resyncForDay(Timestamp day) const;

private:
    std::filesystem::path root_;
    std::filesystem::path requests_;
    std::filesystem::path processed_;
    std::filesystem::path resync_state_;
};


// Manual Control limits from the portfolio-risk config. A manual weight applies to every
// strategy (each on its own share of equity), so the account limits are the strictest
// strategy's, as in the replay. A request above them is rejected, never clipped.
struct ManualRiskConfig {
    std::unordered_map<StrategyID, double> allocation;
    double max_gross_leverage = 0.0;
    double max_asset_weight = 0.0;
};

// Reads the limits and allocations; every strategy of `strategies` must be in the file.
ManualRiskConfig loadManualRiskConfig(const std::string& path, const std::vector<StrategyID>& strategies);

// No manual orders from 23:45 to 00:15 UTC: the daily cycle reconciles, decides and sends
// its orders then.
bool inDailyCycleWindow(std::chrono::system_clock::time_point now);

// "2026-10-14T12:00:00Z" -> time point; nothing when the text has another shape.
std::optional<std::chrono::system_clock::time_point> parseUtcSeconds(const std::string& text);
