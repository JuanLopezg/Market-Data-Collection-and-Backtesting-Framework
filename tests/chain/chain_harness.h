#pragma once

// Runs the PAPER trading chain (historical feeder -> strategy -> risk -> execution state
// -> planner -> gateway -> simulated exchange) as real processes against Docker NATS and
// PostgreSQL, with accelerated business time shared by every service.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "test_support.h"

namespace chain_test {

// One daily bar of the synthetic CSV (date as YYYY-MM-DD).
struct CsvBar {
    std::string date;
    std::string symbol;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

struct ChainSettings {
    std::vector<CsvBar> bars;           // ordered by date
    std::string first_date;             // business time starts at 00:00 UTC of this day;
                                        // earlier CSV days are warm-up history visible at once
    double seconds_per_business_day = 4.0;
    double initial_cash = 100000.0;
    double commission_rate = 0.001;
    int market_warmup_days = 30;
};

using Rows = std::vector<std::vector<std::string>>;

class PaperChain {
public:
    // Starts NATS, PostgreSQL and every service. Throws when infrastructure fails.
    explicit PaperChain(ChainSettings settings);
    ~PaperChain();

    // Runs a SQL query on the chain's PostgreSQL and returns the rows as text.
    Rows query(const std::string& sql) const;

    // Waits until execution state has recorded the equity row of `dateKey` (YYYYMMDD).
    bool waitForEquityDay(long dateKey, std::chrono::seconds timeout) const;

    // Crash simulation: SIGKILL one service, then start it again with the same arguments
    // after `downtime` (real time).
    void crashAndRestart(const std::string& service,
                         std::chrono::milliseconds downtime = std::chrono::milliseconds(0));

    // Newest market day the strategy has published intents for, from its checkpoint table.
    long newestStrategyDay() const;

    // Service logs, for failure messages.
    std::string logs() const;

private:
    void startService(const std::string& name);

    ChainSettings settings_;
    test_support::TempDir dir_;
    std::unique_ptr<test_support::DockerContainer> nats_;
    std::unique_ptr<test_support::DockerContainer> postgres_;
    std::map<std::string, std::string> timeEnvironment_;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> commands_;
    std::map<std::string, std::unique_ptr<test_support::ChildProcess>> processes_;
    std::vector<std::string> startOrder_;
};

} // namespace chain_test
