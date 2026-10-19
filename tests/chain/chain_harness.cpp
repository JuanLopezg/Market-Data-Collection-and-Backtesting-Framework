#include "chain_harness.h"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <libpq-fe.h>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace chain_test {

namespace {

constexpr const char* STREAM = "ALGOTRADING_TEST_RUNTIME";
constexpr const char* CONTROL_STREAM = "ALGOTRADING_TEST_EXCHANGE_CONTROL";
constexpr const char* BACKEND_STREAM = "ALGOTRADING_TEST_EXCHANGE_BACKEND";
constexpr const char* SOURCE_ID = "historical-test";

std::string utcNowWholeSeconds()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

std::string formatNumber(double value)
{
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

} // namespace

PaperChain::PaperChain(ChainSettings settings)
    : settings_(std::move(settings)), dir_("algotrading-chain")
{
    nats_ = test_support::DockerContainer::startNats();
    postgres_ = test_support::DockerContainer::startPostgres();

    // The synthetic historical CSV the feeder reads day by day.
    std::ostringstream csv;
    csv << "date,symbol,open,high,low,close,volume\n";
    for (const auto& bar : settings_.bars)
        csv << bar.date << ',' << bar.symbol << ',' << formatNumber(bar.open) << ',' << formatNumber(bar.high)
            << ',' << formatNumber(bar.low) << ',' << formatNumber(bar.close) << ',' << formatNumber(bar.volume)
            << '\n';
    test_support::writeFile(dir_ / "market.csv", csv.str());

    // One business-time setting for every service: settings.first_date 00:00 UTC is now.
    const double speed = 86400.0 / settings_.seconds_per_business_day;
    timeEnvironment_ = {
        {"ALGOTRADING_TIME_SPEED", formatNumber(speed)},
        {"ALGOTRADING_TIME_REAL_REFERENCE_UTC", utcNowWholeSeconds()},
        {"ALGOTRADING_TIME_SIMULATED_REFERENCE_UTC", settings_.first_date + "T00:00:00Z"},
        {"ALGOTRADING_LOG_QUIET", "0"},
        {"ALGOTRADING_LOG_DEBUG", "1"},
    };

    const std::string db = (dir_ / "market.db").string();
    const std::string nats = nats_->url();
    const std::string pg = postgres_->url();
    const std::string strategyConfig = test_support::sourcePath("config/strategies/pure_rsi_signal.json").string();
    const std::string portfolioConfig = test_support::sourcePath("config/portfolio/pure_rsi_equal_weight.json").string();

    // Same arguments as deploy/historical_replay/docker-compose.yml.
    commands_["feeder"] = {"ALGOTRADING_BIN_HISTORICAL_MARKET_DATA_SERVICE",
        {"--nats-url", nats, "--stream", STREAM, "--historical-data", (dir_ / "market.csv").string(),
         "--market-data-db", db, "--source-id", SOURCE_ID, "--market-top-n", "1000000",
         "--poll-timeout-ms", "50"}};
    commands_["strategy"] = {"ALGOTRADING_BIN_STRATEGY_SERVICE",
        {"--nats-url", nats, "--stream", STREAM, "--strategy-config", strategyConfig, "--market-data-db", db,
         "--market-warmup-days", std::to_string(settings_.market_warmup_days), "--market-top-n", "1000000",
         "--market-data-source", SOURCE_ID, "--postgres", pg}};
    commands_["risk"] = {"ALGOTRADING_BIN_PORTFOLIO_RISK_SERVICE",
        {"--nats-url", nats, "--stream", STREAM, "--portfolio-config", portfolioConfig, "--market-data-db", db,
         "--market-history-buffer-days", "5", "--postgres", pg}};
    commands_["execution"] = {"ALGOTRADING_BIN_EXECUTION_STATE_SERVICE",
        {"--nats-url", nats, "--postgres", pg, "--stream", STREAM, "--exchange-control-stream", CONTROL_STREAM,
         "--market-data-db", db, "--market-data-source", SOURCE_ID, "--initial-cash",
         formatNumber(settings_.initial_cash), "--strategy", "1:Pure_RSI"}};
    commands_["planner"] = {"ALGOTRADING_BIN_ORDER_PLANNER_SERVICE",
        {"--nats-url", nats, "--stream", STREAM, "--postgres", pg}};
    commands_["gateway"] = {"ALGOTRADING_BIN_EXCHANGE_GATEWAY",
        {"--nats-url", nats, "--stream", STREAM, "--control-stream", CONTROL_STREAM, "--backend-stream",
         BACKEND_STREAM, "--mode", "backend", "--poll-timeout-ms", "50"}};
    commands_["sim-exchange"] = {"ALGOTRADING_BIN_SIMULATED_EXCHANGE_SERVICE",
        {"--nats-url", nats, "--stream", STREAM, "--backend-stream", BACKEND_STREAM, "--postgres", pg,
         "--initial-cash", formatNumber(settings_.initial_cash), "--commission-rate",
         formatNumber(settings_.commission_rate), "--poll-timeout-ms", "50"}};

    // Schema first, as market-data-init does, then consumers before the feeder.
    test_support::ChildProcess init(
        test_support::builtExecutable("ALGOTRADING_BIN_HISTORICAL_MARKET_DATA_SERVICE"),
        {"--market-data-db", db, "--init-only"}, dir_ / "logs/init.log", timeEnvironment_);
    const auto initExit = init.waitForExit(std::chrono::seconds(60));
    if (!initExit || *initExit != 0)
        throw std::runtime_error("Market database initialization failed:\n" + init.log());

    startOrder_ = {"sim-exchange", "gateway", "planner", "execution", "risk", "strategy", "feeder"};
    for (const auto& name : startOrder_)
        startService(name);
}

PaperChain::~PaperChain()
{
    // Stop in reverse order so nothing publishes into a stopped consumer.
    for (auto it = startOrder_.rbegin(); it != startOrder_.rend(); ++it)
        if (processes_.count(*it))
            processes_[*it]->stop(std::chrono::seconds(5));
}

void PaperChain::startService(const std::string& name)
{
    const auto& [executableVariable, arguments] = commands_.at(name);
    processes_[name] = std::make_unique<test_support::ChildProcess>(
        test_support::builtExecutable(executableVariable), arguments, dir_ / ("logs/" + name + ".log"),
        timeEnvironment_);
}

void PaperChain::crashAndRestart(const std::string& service, std::chrono::milliseconds downtime)
{
    processes_.at(service)->kill();
    std::this_thread::sleep_for(downtime);
    startService(service);
}

long PaperChain::newestStrategyDay() const
{
    try {
        const Rows rows = query("SELECT COALESCE(MAX(timestamp), 0) FROM strategy_market_update_checkpoint");
        return rows.empty() ? 0 : std::stol(rows[0][0]);
    }
    catch (const std::exception&) {
        return 0;
    }
}

Rows PaperChain::query(const std::string& sql) const
{
    PGconn* connection = PQconnectdb(postgres_->url().c_str());
    if (PQstatus(connection) != CONNECTION_OK) {
        const std::string error = PQerrorMessage(connection);
        PQfinish(connection);
        throw std::runtime_error("Cannot connect to PostgreSQL: " + error);
    }
    PGresult* result = PQexec(connection, sql.c_str());
    Rows rows;
    if (PQresultStatus(result) == PGRES_TUPLES_OK) {
        for (int row = 0; row < PQntuples(result); ++row) {
            std::vector<std::string> values;
            for (int column = 0; column < PQnfields(result); ++column)
                values.emplace_back(PQgetvalue(result, row, column));
            rows.push_back(std::move(values));
        }
    }
    else if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        const std::string error = PQresultErrorMessage(result);
        PQclear(result);
        PQfinish(connection);
        throw std::runtime_error("Query failed: " + error + " in: " + sql);
    }
    PQclear(result);
    PQfinish(connection);
    return rows;
}

bool PaperChain::waitForEquityDay(long dateKey, std::chrono::seconds timeout) const
{
    return test_support::waitUntil(
        [&] {
            try {
                return !query("SELECT 1 FROM trading_equity_daily WHERE date >= " + std::to_string(dateKey)).empty();
            }
            catch (const std::exception&) {
                return false; // the table appears after execution state starts
            }
        },
        timeout, std::chrono::milliseconds(500));
}

std::string PaperChain::logs() const
{
    std::string text;
    for (const auto& name : startOrder_) {
        const std::string log = processes_.at(name)->log();
        // The tail is what explains a failure; whole logs are kept with ALGOTRADING_TEST_KEEP_TMP=1.
        text += "===== " + name + " (last 3000 chars)\n" +
                (log.size() > 3000 ? log.substr(log.size() - 3000) : log) + "\n";
    }
    return text;
}

} // namespace chain_test
