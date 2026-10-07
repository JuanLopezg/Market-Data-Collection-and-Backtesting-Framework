#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "replay_runtime.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "validated/pure_rsi.h"
#include "risk_constraints.h"
#include "time_handler.h"

using namespace Replay;
using namespace MockVenue;

namespace {

struct Args {
    std::filesystem::path csv;
    std::filesystem::path mapping;
    std::filesystem::path durable;
    std::filesystem::path output;
    std::filesystem::path fills_output;
    std::string start_date = "2020-01-01";
    std::string end_date = "2025-10-13";
    std::string profile = "realtest-parity";
    std::string pace_start;
    std::string pace_end;
    double speed = 1500.0;
};

struct DayData {
    std::string date;
    unsigned int yyyymmdd = 0U;
    Timestamp open_timestamp = 0U;
    Timestamp close_timestamp = 0U;
    TimeHandler::TimePoint open_time{};
    TimeHandler::TimePoint close_time{};
    MarketSliceSnapshot slice;
    ExecutionReferencePrices open_prices;
};

std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    std::stringstream stream(line);
    while (std::getline(stream, field, ','))
        fields.push_back(field);
    return fields;
}

Args parseArgs(int argc, char** argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + key);
            return argv[++i];
        };

        if (key == "--csv") args.csv = value();
        else if (key == "--mapping") args.mapping = value();
        else if (key == "--durable") args.durable = value();
        else if (key == "--output") args.output = value();
        else if (key == "--fills-output") args.fills_output = value();
        else if (key == "--start") args.start_date = value();
        else if (key == "--end") args.end_date = value();
        else if (key == "--profile") args.profile = value();
        else if (key == "--pace-start") args.pace_start = value();
        else if (key == "--pace-end") args.pace_end = value();
        else if (key == "--speed") args.speed = std::stod(value());
        else throw std::invalid_argument("unknown argument: " + key);
    }

    if (args.csv.empty() || args.mapping.empty() ||
        args.durable.empty() || args.output.empty() || args.fills_output.empty())
        throw std::invalid_argument("--csv --mapping --durable --output --fills-output are required");
    if (!std::isfinite(args.speed) || args.speed <= 0.0)
        throw std::invalid_argument("speed must be finite and positive");
    if (args.profile != "mock-default" && args.profile != "realtest-parity")
        throw std::invalid_argument("profile must be mock-default or realtest-parity");
    if (args.start_date > args.end_date)
        throw std::invalid_argument("start date must not be after end date");
    if (args.pace_start.empty() != args.pace_end.empty())
        throw std::invalid_argument("pace-start and pace-end must be provided together");
    if (!args.pace_start.empty() &&
        (args.pace_start < args.start_date || args.pace_end > args.end_date || args.pace_start > args.pace_end))
        throw std::invalid_argument("paced range must be inside replay range");
    return args;
}

unsigned int dateInt(const std::string& date)
{
    if (date.size() != 10U || date[4] != '-' || date[7] != '-')
        throw std::invalid_argument("invalid YYYY-MM-DD date: " + date);
    return static_cast<unsigned int>(
        std::stoul(date.substr(0,4) + date.substr(5,2) + date.substr(8,2)));
}

TimeHandler::TimePoint dateTime(
    const std::string& date,
    int hour,
    int minute,
    int second)
{
    const int year = std::stoi(date.substr(0,4));
    const unsigned month = static_cast<unsigned>(std::stoi(date.substr(5,2)));
    const unsigned day = static_cast<unsigned>(std::stoi(date.substr(8,2)));
    const std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };
    if (!ymd.ok())
        throw std::invalid_argument("invalid calendar date: " + date);
    return std::chrono::sys_days{ymd}
        + std::chrono::hours{hour}
        + std::chrono::minutes{minute}
        + std::chrono::seconds{second};
}

std::unordered_map<std::string,std::string> loadMapping(
    const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("unable to open source mapping: " + path.string());

    std::string line;
    if (!std::getline(in,line) || line != "source_symbol,canonical_asset")
        throw std::runtime_error("unexpected source mapping header");

    std::unordered_map<std::string,std::string> mapping;
    while (std::getline(in,line)) {
        if (line.empty()) continue;
        const auto fields = splitCsv(line);
        if (fields.size() != 2U || fields[0].empty() || fields[1].empty())
            throw std::runtime_error("invalid source mapping row");
        if (!mapping.emplace(fields[0],fields[1]).second)
            throw std::runtime_error("duplicate source symbol mapping: " + fields[0]);
        const auto* entry = findByCanonicalAssetExact(fields[1]);
        if (entry == nullptr || !entry->enabled)
            throw std::runtime_error("mapping target absent/disabled in MOCK catalog: " + fields[1]);
    }
    if (mapping.empty())
        throw std::runtime_error("source mapping cannot be empty");
    return mapping;
}

double campaignVolume(const Args& args, double raw_volume)
{
    if (!std::isfinite(raw_volume) || raw_volume < 0.0)
        throw std::runtime_error("invalid historical volume");

    if (args.profile == "realtest-parity") {
        // Affine transform v' = 0.1*v + 1e10. For every fully warmed SMA(25),
        // SMA(v') = 0.1*SMA(v) + 1e10, so liquidity ranking order is unchanged.
        // The floor intentionally removes legacy-vs-MOCK liquidity-cap differences
        // from the RealTest comparison while keeping values inside Step51 uint64 grid.
        return raw_volume * 0.1 + 10000000000.0;
    }
    return raw_volume;
}

std::map<std::string,DayData> loadDays(
    const Args& args,
    const std::unordered_map<std::string,std::string>& mapping,
    std::size_t* out_rows,
    std::set<std::string>* out_source_symbols)
{
    std::ifstream in(args.csv);
    if (!in)
        throw std::runtime_error("unable to open historical CSV: " + args.csv.string());

    std::string line;
    if (!std::getline(in,line) || line != "date,symbol,open,high,low,close,volume")
        throw std::runtime_error("unexpected historical CSV header");

    std::map<std::string,DayData> days;
    std::size_t rows = 0U;
    std::set<std::string> symbols;

    while (std::getline(in,line)) {
        if (line.empty()) continue;
        const auto fields = splitCsv(line);
        if (fields.size() != 7U)
            throw std::runtime_error("invalid historical CSV row");

        const std::string& date = fields[0];
        if (date < args.start_date) continue;
        if (date > args.end_date) break;

        const auto map_it = mapping.find(fields[1]);
        if (map_it == mapping.end())
            throw std::runtime_error("UNMAPPED historical source symbol: " + fields[1]);

        DayData& day = days[date];
        if (day.date.empty()) {
            day.date = date;
            day.yyyymmdd = dateInt(date);
            day.open_timestamp = day.yyyymmdd * 10U + 1U;
            day.close_timestamp = day.yyyymmdd * 10U + 9U;
            day.open_time = dateTime(date,0,0,0);
            day.close_time = dateTime(date,23,59,59);
            day.slice.timestamp = day.close_timestamp;
        }

        MarketBarSnapshot value;
        value.coin = map_it->second;
        value.bar.open = std::stod(fields[2]);
        value.bar.high = std::stod(fields[3]);
        value.bar.low = std::stod(fields[4]);
        value.bar.close = std::stod(fields[5]);
        value.bar.volume = campaignVolume(args,std::stod(fields[6]));

        day.slice.bars.push_back(value);
        day.open_prices.set(value.coin,value.bar.open);
        ++rows;
        symbols.insert(fields[1]);
    }

    if (days.empty())
        throw std::runtime_error("acceptance window contains no historical days");

    *out_rows = rows;
    *out_source_symbols = std::move(symbols);
    return days;
}

StrategySignalEngine makeStrategyEngine()
{
    StrategySignalPortfolio strategies;
    strategies.emplace_back(
        1U,
        std::make_unique<StrategyPureRSI>(
            10U,
            std::make_unique<TopNLiquidityUniverse>(
                IndicatorSpec{IndicatorKind::SMA,PriceField::Volume,25U,0U},
                20U,
                true,
                true),
            std::make_unique<IndicatorRanker>(
                IndicatorSpec{IndicatorKind::RSI,PriceField::Close,7U,0U},
                true,
                true),
            1000000U,
            7U,
            80.0,
            70.0));
    return StrategySignalEngine(std::move(strategies));
}

PortfolioRiskEngine makeRiskEngine()
{
    std::vector<PortfolioRiskStrategyConfig> configs;
    configs.emplace_back(
        1U,
        "Pure_RSI",
        1.0,
        std::make_unique<EqualWeightSizer>(0.10),
        RiskConstraints(1.50,1.50),
        std::make_unique<EntryExitOnlyRebalancePolicy>());
    return PortfolioRiskEngine(std::move(configs));
}

std::string reconciliationState(ReconciliationState state)
{
    switch (state) {
    case ReconciliationState::Pending: return "PENDING";
    case ReconciliationState::Clean: return "CLEAN";
    case ReconciliationState::Blocked: return "BLOCKED";
    }
    return "UNKNOWN";
}

void exportFillsForRealTest(
    const std::filesystem::path& path,
    const std::vector<UserStreamEnvelope>& stream,
    const std::unordered_map<std::string,std::string>& source_to_canonical)
{
    std::unordered_map<std::string,std::string> canonical_to_source;
    for (const auto& pair : source_to_canonical) {
        if (!canonical_to_source.emplace(pair.second,pair.first).second)
            throw std::runtime_error("duplicate canonical reverse mapping: " + pair.second);
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("unable to open fills output: " + path.string());

    out << "fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission\n";
    out << std::setprecision(17);

    std::uint64_t fill_id = 0U;
    for (const auto& envelope : stream) {
        if (!std::holds_alternative<VenueContracts::V1::Fill>(envelope.event))
            continue;

        const auto& fill = std::get<VenueContracts::V1::Fill>(envelope.event);
        const auto source = canonical_to_source.find(fill.instrument.market.canonical_asset);
        if (source == canonical_to_source.end())
            throw std::runtime_error(
                "canonical fill asset lacks source reverse mapping: " +
                fill.instrument.market.canonical_asset);

        ++fill_id;
        const Timestamp legacy_timestamp = fill.timestamp / 10U;
        out << fill_id << ','
            << fill.local_order_id << ','
            << fill.strategy_id << ','
            << legacy_timestamp << ','
            << source->second << ','
            << (fill.side == VenueContracts::V1::Side::Buy ? 0 : 1) << ','
            << fill.quantity << ','
            << fill.price << ",0\n";
    }
}

std::string jsonEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size()+8U);
    for (const char ch : input) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Args args = parseArgs(argc,argv);
        const auto mapping = loadMapping(args.mapping);

        std::size_t source_rows = 0U;
        std::set<std::string> window_symbols;
        auto days = loadDays(args,mapping,&source_rows,&window_symbols);

        StrategySignalEngine strategy = makeStrategyEngine();
        PortfolioRiskEngine risk = makeRiskEngine();
        NotionalOrderPlanner planner;

        MockChaosConfig chaos;
        chaos.auto_submit_faults = false;
        chaos.submit_limit = 100000U;
        chaos.cancel_limit = 100000U;
        chaos.modify_limit = 100000U;
        chaos.market_source_limit = 1000000U;
        chaos.reconcile_limit = 100000U;
        chaos.stream_read_limit = 1000000U;

        std::error_code ignored;
        std::filesystem::remove_all(args.durable,ignored);

        MatchingFillConfig matching;
        if (args.profile == "realtest-parity") {
            matching.participation_ppm = 1000000U;
            matching.max_adverse_slippage_ppm = 0U;
        }

        MockExchange adapter(args.durable,chaos,matching);
        ReplayRuntime replay(
            strategy,
            risk,
            planner,
            {1U},
            adapter);

        std::unique_ptr<TimeHandler> release_clock;

        for (const auto& pair : days) {
            const DayData& day = pair.second;
            const bool paced =
                !args.pace_start.empty() &&
                day.date >= args.pace_start &&
                day.date <= args.pace_end;

            if (paced && !release_clock) {
                const auto real_reference = TimeHandler::Clock::now();
                const auto bias = day.open_time - real_reference;
                release_clock = std::make_unique<TimeHandler>(
                    args.speed,bias,real_reference);
            }

            if (paced)
                release_clock->sleepUntil(day.open_time);
            replay.onExecutionOpen(day.open_timestamp,day.open_prices);

            if (paced)
                release_clock->sleepUntil(day.close_time);
            replay.onClosedSlice(day.slice);
        }

        const Timestamp final_timestamp = days.rbegin()->second.close_timestamp;
        const auto evidence = replay.finalizeEvidence(final_timestamp);
        const auto account = adapter.chaos().runtime().account().accountSnapshot(final_timestamp);
        const auto open_orders = adapter.chaos().runtime().account().openOrdersSnapshot(
            adapter.chaos().runtime().lifecycle(),final_timestamp);
        const auto& reconciliation = adapter.chaos().lastReconciliation();
        const auto margin = adapter.chaos().runtime().account().marginState(final_timestamp);

        exportFillsForRealTest(
            args.fills_output,
            adapter.chaos().runtime().stream(),
            mapping);

        const bool pass =
            reconciliation.clean() &&
            replay.routeSafe() &&
            evidence.closed_slices == days.size() &&
            evidence.signal_batches == days.size() &&
            evidence.decision_batches == days.size() &&
            evidence.planning_batches == days.size() &&
            evidence.canonical_fills > 0U &&
            evidence.canonical_accounting_events == evidence.canonical_fills &&
            !evidence.full_run_fingerprint.empty();

        std::filesystem::create_directories(args.output.parent_path());
        std::ofstream out(args.output);
        if (!out)
            throw std::runtime_error("unable to open output summary: " + args.output.string());

        out << std::setprecision(15);
        out << "{\n";
        out << "  \"result\": \"" << (pass ? "PASS" : "FAIL") << "\",\n";
        out << "  \"campaign\": \"STEP58A_FULL_SYSTEM_REPLAY_REALTEST\",\n";
        out << "  \"profile\": \"" << jsonEscape(args.profile) << "\",\n";
        out << "  \"releaseMode\": \"" << (args.pace_start.empty() ? "UNPACED_BUSINESS_TIME" : "TIMEHANDLER_PACED_SEGMENT") << "\",\n";
        out << "  \"paceStart\": \"" << jsonEscape(args.pace_start) << "\",\n";
        out << "  \"paceEnd\": \"" << jsonEscape(args.pace_end) << "\",\n";
        out << "  \"firstDate\": \"" << jsonEscape(days.begin()->first) << "\",\n";
        out << "  \"lastDate\": \"" << jsonEscape(days.rbegin()->first) << "\",\n";
        out << "  \"sourceRows\": " << source_rows << ",\n";
        out << "  \"days\": " << days.size() << ",\n";
        out << "  \"sourceSymbolsInWindow\": " << window_symbols.size() << ",\n";
        out << "  \"mappingEntries\": " << mapping.size() << ",\n";
        out << "  \"timeHandlerSpeed\": " << args.speed << ",\n";
        out << "  \"volumeTransform\": \"" << (args.profile == "realtest-parity" ? "v_prime=0.1*v+10000000000" : "IDENTITY") << "\",\n";
        out << "  \"matcherParticipationPpm\": " << matching.participation_ppm << ",\n";
        out << "  \"matcherMaxAdverseSlippagePpm\": " << matching.max_adverse_slippage_ppm << ",\n";
        out << "  \"strategy\": \"Pure_RSI\",\n";
        out << "  \"plannedSubmits\": " << evidence.planned_submits << ",\n";
        out << "  \"plannedCancels\": " << evidence.planned_cancels << ",\n";
        out << "  \"canonicalFills\": " << evidence.canonical_fills << ",\n";
        out << "  \"canonicalAccountingEvents\": " << evidence.canonical_accounting_events << ",\n";
        out << "  \"knownOrders\": " << adapter.chaos().runtime().lifecycle().orders().size() << ",\n";
        out << "  \"openOrders\": " << open_orders.orders.size() << ",\n";
        out << "  \"finalPositions\": " << account.positions.size() << ",\n";
        out << "  \"finalEquity\": " << account.equity << ",\n";
        out << "  \"settledCash\": " << margin.settled_cash << ",\n";
        out << "  \"realizedPnl\": " << margin.realized_pnl << ",\n";
        out << "  \"unrealizedPnl\": " << margin.unrealized_pnl << ",\n";
        out << "  \"grossExposure\": " << margin.gross_exposure << ",\n";
        out << "  \"feesPaid\": " << signedScaledToDouble(adapter.chaos().runtime().account().feesPaidUnits(),kMockMoneyScale) << ",\n";
        out << "  \"reconciliation\": \"" << reconciliationState(reconciliation.state) << "\",\n";
        out << "  \"reconciliationIssues\": " << reconciliation.issues.size() << ",\n";
        out << "  \"routeSafe\": " << (replay.routeSafe() ? "true" : "false") << ",\n";
        out << "  \"economicFingerprint\": \"" << evidence.economic_fingerprint << "\",\n";
        out << "  \"streamFingerprint\": \"" << evidence.stream_fingerprint << "\",\n";
        out << "  \"ledgerHeadHash\": \"" << evidence.ledger_head_hash << "\",\n";
        out << "  \"chaosEvidenceFingerprint\": \"" << evidence.chaos_evidence_fingerprint << "\",\n";
        out << "  \"fullRunFingerprint\": \"" << evidence.full_run_fingerprint << "\",\n";
        out << "  \"step56RuntimeFingerprint\": \"" << evidence.step56_runtime_fingerprint << "\"\n";
        out << "}\n";
        out.close();

        std::cout << "STEP58A_RESULT=" << (pass ? "PASS" : "FAIL") << "\n";
        std::cout << "STEP58A_DAYS=" << days.size() << "\n";
        std::cout << "STEP58A_FILLS=" << evidence.canonical_fills << "\n";
        std::cout << "STEP58A_FULL_RUN_FINGERPRINT=" << evidence.full_run_fingerprint << "\n";
        std::cout << "STEP58A_SUMMARY=" << args.output.string() << "\n";
        return pass ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << "STEP58A_FATAL=" << error.what() << "\n";
        return 2;
    }
}
