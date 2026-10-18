#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "replay_runtime.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "mock/decimal.h"
#include "validated/pure_rsi.h"
#include "risk_constraints.h"
#include "time_handler.h"
#include "logger.h"

using namespace Replay;
using namespace MockVenue;
using namespace VenueContracts::V1;

namespace {

// Daily equity points kept in the dashboard state (display only, not economics).
// Large enough for the whole 2020-2025 history (~2100 days) with room to spare.
constexpr std::size_t MAX_DASHBOARD_EQUITY_POINTS = 4000;

struct Args {
    std::filesystem::path csv;
    std::filesystem::path mapping;
    std::filesystem::path durable;
    std::filesystem::path output;
    std::filesystem::path fills_output;
    std::filesystem::path dashboard_state_dir;

    std::string mode = "system";
    std::string start_date = "2020-01-01";
    std::string end_date = "2025-10-13";
    std::string profile = "realtest-parity";

    std::string pace_start;
    std::string pace_end;
    std::string visual_start;
    std::string visual_end;

    double speed = 1500.0;
    int ui_delay_ms = 0;

    std::filesystem::path checkpoint_state;
    int checkpoint_every = 0;
    int stop_after_days = 0;
    bool resume = false;
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

// One dashboard point per replayed day (display only; never feeds trading).
struct EquityPoint {
    std::string label;
    double equity = 0.0;
    double balance = 0.0;          // equity without open-position PnL
    double gross = 0.0;            // sum of |position value| (leverage = gross / equity)
    std::string close_loop;        // reconciliation state that day: CLEAN, PENDING, BLOCKED
};

// One strategy's view of one asset: its signal, its own indicator and its risk evaluation.
// Weights are fractions of that strategy's capital.
struct PipelineStrategyLine {
    StrategyID strategy_id = 0U;
    std::string name;
    double capital = 0.0;           // strategy reference capital (USD)
    double share = 0.0;             // capital / total capital of all strategies
    double signal = 0.0;            // [-1, +1]
    std::string detail;             // strategy-specific indicator, e.g. "RSI(7) 84.2 · #3 in universe"
    std::string rule;               // its entry/exit rule, e.g. "Enter RSI(7) > 80, exit < 70"
    double sized_weight = 0.0;      // sizer output
    double capped_weight = 0.0;     // after the per-asset cap
    double approved_weight = 0.0;   // after the gross scale
    double max_asset = 0.0;
    double max_gross = 0.0;
    double gross_after_asset_cap = 0.0;
    double gross_scale = 1.0;
    std::string sizer;
    std::string action = "HOLD";    // rebalance policy: HOLD, FLAT or TARGET
};

// One asset of the latest close decision, stage by stage (display only; never feeds trading).
// Account-level figures combine the strategies by capital share.
struct PipelineAsset {
    std::string asset;
    double close = 0.0;
    double liquidity = std::numeric_limits<double>::quiet_NaN();   // SMA(volume, 25)
    unsigned int liquidity_rank = 0U;                               // 0 = not rankable
    bool in_universe = false;                                       // inside the liquidity top N
    std::vector<PipelineStrategyLine> strategies;
    double signal = 0.0;            // aggregated: sum of share * strategy signal, in [-1, +1]
    double sized_weight = 0.0;      // sum of share * strategy weight (fraction of total capital)
    double capped_weight = 0.0;
    double approved_weight = 0.0;
    std::string action = "HOLD";    // combined rebalance action: HOLD, FLAT or TARGET
    bool has_target = false;        // HOLD emits no new target
    double target_usd = 0.0;
    double current_qty = 0.0;       // strategy position at decision time
    double current_usd = 0.0;
    std::vector<PlannedNotionalOrder> orders;
};

struct PipelineView {
    bool available = false;
    std::string date;
    Timestamp decision_timestamp = 0U;
    double total_capital = 0.0;     // sum of the strategies' reference capital
    double approved_gross = 0.0;    // sum of share * strategy approved gross
    std::size_t strategy_count = 0U;
    std::size_t observed_assets = 0U;
    std::size_t universe_size = 0U;
    std::size_t active_signals = 0U;
    std::size_t cancel_orders = 0U;
    std::vector<PipelineAsset> assets;
};

// Operator manual rebalance (Manual Control page). dashboard-api queues confirmed
// requests as files in <dashboard state dir>/requests. The replay executes the newest one
// at the next price it has (the open for overnight requests, the close for daytime
// ones); the end-of-day decision then re-syncs the account to the strategy targets.
struct ManualControlView {
    bool accepting = false;                  // replay running and picking up requests
    double max_gross_leverage = 0.0;         // risk limits a manual target must respect
    double max_asset_weight = 0.0;
    bool resync_pending = false;             // manual positions held until the next close
    std::string last_executed;               // "<date> open|close"
    std::map<std::string, double> weights;   // last executed target, fraction of equity
    std::string last_request_id;
    std::string last_actor;
    std::string last_status;                 // EXECUTED, REJECTED or SUPERSEDED
    std::string last_detail;
    std::string last_order_ids;              // "first-last" local order ids, empty if none
};

// What the dashboard keeps of the latest close decision and the manual target.
struct DashboardDecision {
    std::map<std::string, double> targets;   // approved exposure per asset (USD)
    PipelineView pipeline;
    ManualControlView manual;
    // ReplayRuntime::routeSafe(): venue/recovery faults block new orders. Unlike
    // canRouteNewSubmit() it does not require a reconciliation after the latest event,
    // because the runtime reconciles right before every submit.
    bool route_safe = true;
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

        if (key == "--mode") args.mode = value();
        else if (key == "--csv") args.csv = value();
        else if (key == "--mapping") args.mapping = value();
        else if (key == "--durable") args.durable = value();
        else if (key == "--output") args.output = value();
        else if (key == "--fills-output") args.fills_output = value();
        else if (key == "--dashboard-state-dir") args.dashboard_state_dir = value();
        else if (key == "--start") args.start_date = value();
        else if (key == "--end") args.end_date = value();
        else if (key == "--profile") args.profile = value();
        else if (key == "--pace-start") args.pace_start = value();
        else if (key == "--pace-end") args.pace_end = value();
        else if (key == "--visual-start") args.visual_start = value();
        else if (key == "--visual-end") args.visual_end = value();
        else if (key == "--speed") args.speed = std::stod(value());
        else if (key == "--ui-delay-ms") args.ui_delay_ms = std::stoi(value());
        else if (key == "--checkpoint-state") args.checkpoint_state = value();
        else if (key == "--checkpoint-every") args.checkpoint_every = std::stoi(value());
        else if (key == "--stop-after-days") args.stop_after_days = std::stoi(value());
        else if (key == "--resume") args.resume = true;
        else if (key == "--help" || key == "-h") {
            std::cout
                << "Usage: algotrading_replay_full --mode system|dashboard "
                   "--csv PATH --mapping PATH --durable DIR --output JSON "
                   "--fills-output CSV [--dashboard-state-dir DIR] "
                   "[--start YYYY-MM-DD --end YYYY-MM-DD] "
                   "[--profile realtest-parity|mock-default] "
                   "[--speed N --pace-start DATE --pace-end DATE] "
                   "[--checkpoint-state PATH --checkpoint-every N] "
                   "[--stop-after-days N] [--resume] "
                   "[--visual-start DATE --visual-end DATE --ui-delay-ms N]\n";
            std::exit(0);
        }
        else throw std::invalid_argument("unknown argument: " + key);
    }

    if (args.mode != "system" && args.mode != "dashboard")
        throw std::invalid_argument("--mode must be system or dashboard");
    if (args.csv.empty() || args.mapping.empty() || args.durable.empty() ||
        args.output.empty() || args.fills_output.empty())
        throw std::invalid_argument(
            "--csv --mapping --durable --output --fills-output are required");
    if (args.mode == "dashboard" && args.dashboard_state_dir.empty())
        throw std::invalid_argument(
            "--dashboard-state-dir is required in dashboard mode");
    if (!std::isfinite(args.speed) || args.speed <= 0.0)
        throw std::invalid_argument("speed must be finite and positive");
    if (args.ui_delay_ms < 0)
        throw std::invalid_argument("ui-delay-ms must be non-negative");
    if (args.profile != "mock-default" && args.profile != "realtest-parity")
        throw std::invalid_argument("profile must be mock-default or realtest-parity");
    if (args.start_date > args.end_date)
        throw std::invalid_argument("start date must not be after end date");
    if (args.pace_start.empty() != args.pace_end.empty())
        throw std::invalid_argument("pace-start and pace-end must be provided together");
    if (args.checkpoint_every < 0 || args.stop_after_days < 0)
        throw std::invalid_argument("checkpoint/stop day counts must be non-negative");
    if ((args.resume || args.checkpoint_every > 0 || args.stop_after_days > 0) &&
        args.checkpoint_state.empty())
        throw std::invalid_argument(
            "--checkpoint-state is required for checkpoint/restart mode");
    if (args.resume && args.stop_after_days > 0)
        throw std::invalid_argument(
            "--resume and --stop-after-days cannot be used together");
    if (!args.pace_start.empty() &&
        (args.pace_start < args.start_date || args.pace_end > args.end_date ||
         args.pace_start > args.pace_end))
        throw std::invalid_argument("paced range must be inside replay range");

    if (args.mode == "dashboard") {
        if (args.visual_start.empty() && args.visual_end.empty()) {
            args.visual_start = args.start_date;
            args.visual_end = args.end_date;
        }
        if (args.visual_start.empty() != args.visual_end.empty())
            throw std::invalid_argument(
                "visual-start and visual-end must be provided together");
        if (args.visual_start < args.start_date || args.visual_end > args.end_date ||
            args.visual_start > args.visual_end)
            throw std::invalid_argument("visual range must be inside replay range");
    }
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

double canonicalHistoricalVolume(double raw_volume)
{
    if (!std::isfinite(raw_volume) || raw_volume < 0.0)
        throw std::runtime_error("invalid historical volume");
    return raw_volume;
}

void matchRealTestParityAtExecutionOpen(
    MockExchange& adapter,
    const DayData& day)
{
    // Reproduce the historical research/RealTest execution contract without
    // changing Strategy data: orders execute at OPEN(T+1), while the matcher
    // is configured to ignore bar volume as a fill-capacity constraint.
    for (const auto& value : day.slice.bars) {
        MarketBarObservation observation;
        observation.canonical_asset = value.coin;
        observation.event_time = day.open_timestamp;

        const double open = day.open_prices.get(value.coin);
        observation.bar.open = open;
        observation.bar.high = open;
        observation.bar.low = open;
        observation.bar.close = open;
        observation.bar.volume = value.bar.volume;

        const auto result = adapter.processMarketBar(observation);
        if (result.status ==
                MockVenue::ChaosStatus::VenueUnavailable ||
            result.status ==
                MockVenue::ChaosStatus::OutOfOrderRejected)
            throw std::runtime_error(
                "RealTest parity open matching was rejected");
    }

    // Historical RealTest execution is all-or-nothing at the next open.
    // A parity order must never leak into a later day.
    for (const auto& pair :
         adapter.chaos().runtime().lifecycle().orders()) {
        const auto& stored = pair.second;
        if (stored.active() &&
            stored.intent.active_from <= day.open_timestamp)
            throw std::runtime_error(
                "RealTest parity order remained active after execution open");
    }
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
        value.bar.volume = canonicalHistoricalVolume(std::stod(fields[6]));

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

// Pure RSI replay configuration. The dashboard pipeline view reads the same values.
const IndicatorSpec LIQUIDITY_SPEC{IndicatorKind::SMA,PriceField::Volume,25U,0U};
const IndicatorSpec RSI_SPEC{IndicatorKind::RSI,PriceField::Close,7U,0U};
constexpr unsigned int LIQUIDITY_TOP_N = 20U;
constexpr unsigned int MAX_ACTIVE_SIGNALS = 10U;
constexpr double RSI_ENTRY = 80.0;
constexpr double RSI_EXIT = 70.0;

StrategySignalEngine makeStrategyEngine()
{
    StrategySignalPortfolio strategies;
    strategies.emplace_back(
        1U,
        std::make_unique<StrategyPureRSI>(
            MAX_ACTIVE_SIGNALS,
            std::make_unique<TopNLiquidityUniverse>(
                LIQUIDITY_SPEC,
                LIQUIDITY_TOP_N,
                true,
                true),
            std::make_unique<IndicatorRanker>(
                RSI_SPEC,
                true,
                true),
            1000000U,
            RSI_SPEC.length,
            RSI_ENTRY,
            RSI_EXIT));
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

const char* rebalanceActionText(RebalanceAction action)
{
    switch (action) {
    case RebalanceAction::Hold: return "HOLD";
    case RebalanceAction::Flat: return "FLAT";
    case RebalanceAction::TargetWeight: return "TARGET";
    }
    return "UNKNOWN";
}

// Dashboard Pipeline view of the decision just taken at close(T). Per asset it keeps each
// strategy's signal, own indicator and risk evaluation, plus the account-level combination
// by capital share, the DecisionBatch target and the pending plan. Universe membership and
// ranks come from the same selector/ranker classes the strategy uses. Display only:
// nothing here feeds back into trading.
PipelineView dashboardPipeline(
    const ReplayRuntime& replay,
    const StrategySignalEngine& strategy,
    const MarketSliceSnapshot& slice,
    const std::string& date)
{
    PipelineView view;
    if (!replay.hasPendingPlan() || !replay.lastSignalBatch() || !replay.lastDecisionBatch() ||
        replay.lastRiskEvaluations().empty())
        return view;

    const Timestamp ts = slice.timestamp;
    const auto& indicators = strategy.indicators();
    const auto& evaluations = replay.lastRiskEvaluations();
    const auto& plan = replay.pendingPlan();

    view.available = true;
    view.date = date;
    view.decision_timestamp = ts;
    view.cancel_orders = plan.cancel_order_ids.size();
    view.strategy_count = evaluations.size();
    for (const auto& evaluation : evaluations)
        view.total_capital += evaluation.reference_capital;
    // Capital share of each strategy: how much its signal and weights count in the account.
    auto share = [&](const PortfolioRiskEvaluation& evaluation) {
        return view.total_capital > 0.0 ? evaluation.reference_capital / view.total_capital
                                        : 1.0 / static_cast<double>(evaluations.size());
    };
    for (const auto& evaluation : evaluations)
        view.approved_gross += share(evaluation) *
            evaluation.constraints.gross_after_asset_cap * evaluation.constraints.gross_scale;

    CoinBarMap bars;
    for (const auto& b : slice.bars) {
        BarData bar;
        bar.open = b.bar.open;
        bar.high = b.bar.high;
        bar.low = b.bar.low;
        bar.close = b.bar.close;
        bar.volume = b.bar.volume;
        bars[b.coin] = bar;
    }
    view.observed_assets = bars.size();

    std::map<std::string, unsigned int> liquidity_rank;
    for (const auto& ranked : IndicatorRanker(LIQUIDITY_SPEC, true, true).rank(bars, ts, indicators))
        liquidity_rank[ranked.coin] = ranked.rank;
    const CoinBarMap universe =
        TopNLiquidityUniverse(LIQUIDITY_SPEC, LIQUIDITY_TOP_N, true, true).select(bars, ts, indicators);
    std::map<std::string, unsigned int> rsi_rank;
    for (const auto& ranked : IndicatorRanker(RSI_SPEC, true, true).rank(universe, ts, indicators))
        rsi_rank[ranked.coin] = ranked.rank;
    view.universe_size = universe.size();

    std::map<StrategyID, const StrategySignalIntent*> intents;
    std::set<std::string> signalled;
    for (const auto& intent : replay.lastSignalBatch()->strategies) {
        intents[intent.strategy_id] = &intent;
        for (const auto& [coin, value] : intent.signals)
            if (std::abs(value) > 1e-12) {
                signalled.insert(coin);
                ++view.active_signals;
            }
    }

    std::map<std::string, double> current;
    for (const auto& [strategy_id, positions] : replay.strategyPositions())
        for (const auto& [coin, quantity] : positions.values())
            current[coin] += quantity;

    std::map<std::string, double> targets;
    for (const auto& intent : replay.lastDecisionBatch()->strategies)
        for (const auto& [coin, usd] : intent.target_notional_usd)
            targets[coin] += usd;

    std::map<std::string, std::vector<PlannedNotionalOrder>> orders;
    for (const auto& order : plan.submit_orders)
        orders[order.coin].push_back(order);

    // Rows: the universe plus anything with a signal, a position, a decision or an order.
    std::set<std::string> names;
    for (const auto& [coin, bar] : universe) names.insert(coin);
    names.insert(signalled.begin(), signalled.end());
    for (const auto& [coin, quantity] : current) if (quantity != 0.0) names.insert(coin);
    for (const auto& evaluation : evaluations)
        for (const auto& [coin, decision] : evaluation.rebalance_decisions)
            if (decision.action != RebalanceAction::Hold) names.insert(coin);
    for (const auto& [coin, list] : orders) names.insert(coin);

    for (const auto& coin : names) {
        PipelineAsset row;
        row.asset = coin;
        const auto bar = bars.find(coin);
        row.close = bar == bars.end() ? 0.0 : bar->second.close;
        if (indicators.has(coin, ts, LIQUIDITY_SPEC))
            row.liquidity = indicators.value(coin, ts, LIQUIDITY_SPEC);
        if (const auto it = liquidity_rank.find(coin); it != liquidity_rank.end()) row.liquidity_rank = it->second;
        row.in_universe = universe.count(coin) > 0U;

        bool any_target = false, any_flat = false;
        for (const auto& evaluation : evaluations) {
            PipelineStrategyLine line;
            line.strategy_id = evaluation.strategy_id;
            line.name = evaluation.strategy_name;
            line.capital = evaluation.reference_capital;
            line.share = share(evaluation);
            if (const auto it = intents.find(evaluation.strategy_id); it != intents.end()) {
                const auto signal = it->second->signals.find(coin);
                line.signal = signal == it->second->signals.end() ? 0.0 : signal->second;
            }
            // Strategy-specific indicator; only Pure RSI exists today.
            if (line.name == "Pure_RSI") {
                std::ostringstream detail;
                detail << "RSI(" << RSI_SPEC.length << ") ";
                if (indicators.has(coin, ts, RSI_SPEC))
                    detail << std::fixed << std::setprecision(1) << indicators.value(coin, ts, RSI_SPEC);
                else
                    detail << "n/a";
                if (const auto it = rsi_rank.find(coin); it != rsi_rank.end())
                    detail << " · #" << it->second << " by RSI in its universe";
                else
                    detail << " · outside its universe";
                line.detail = detail.str();
                std::ostringstream rule;
                rule << "Top " << LIQUIDITY_TOP_N << " by SMA(volume, " << LIQUIDITY_SPEC.length
                     << "), enter RSI(" << RSI_SPEC.length << ") > " << RSI_ENTRY
                     << ", exit < " << RSI_EXIT << ", max " << MAX_ACTIVE_SIGNALS << " signals";
                line.rule = rule.str();
            }
            line.sizer = portfolioSizerKindName(evaluation.sizer_kind);
            line.max_asset = evaluation.max_asset_weight;
            line.max_gross = evaluation.max_gross_leverage;
            line.gross_after_asset_cap = evaluation.constraints.gross_after_asset_cap;
            line.gross_scale = evaluation.constraints.gross_scale;
            if (evaluation.sizing_available) {
                line.sized_weight = evaluation.sized_weights.get(coin);
                line.capped_weight = evaluation.constraints.asset_capped_weights.get(coin);
                line.approved_weight = evaluation.approved_weights.get(coin);
            }
            if (const auto it = evaluation.rebalance_decisions.find(coin); it != evaluation.rebalance_decisions.end())
                line.action = rebalanceActionText(it->second.action);
            any_target = any_target || line.action == "TARGET";
            any_flat = any_flat || line.action == "FLAT";

            row.signal += line.share * line.signal;
            row.sized_weight += line.share * line.sized_weight;
            row.capped_weight += line.share * line.capped_weight;
            row.approved_weight += line.share * line.approved_weight;
            row.strategies.push_back(std::move(line));
        }
        row.action = any_target ? "TARGET" : any_flat ? "FLAT" : "HOLD";
        if (const auto it = targets.find(coin); it != targets.end()) {
            row.has_target = true;
            row.target_usd = it->second;
        }
        row.current_qty = current[coin];
        row.current_usd = row.current_qty * row.close;
        if (const auto it = orders.find(coin); it != orders.end()) row.orders = it->second;
        view.assets.push_back(std::move(row));
    }
    return view;
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

void exportParityFillsForRealTest(
    const std::filesystem::path& path,
    const std::vector<::Fill>& fills,
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
    for (const auto& fill : fills) {
        const auto source = canonical_to_source.find(fill.coin);
        if (source == canonical_to_source.end())
            throw std::runtime_error(
                "parity fill asset lacks source reverse mapping: " + fill.coin);

        ++fill_id;
        const Timestamp fill_date = fill.timestamp / 10U; // YYYYMMDD without the open/close phase digit
        out << fill_id << ','
            << fill.order_id << ','
            << fill.strategy_id << ','
            << fill_date << ','
            << source->second << ','
            << (fill.side == OrderSide::Buy ? 0 : 1) << ','
            << fill.quantity << ','
            << fill.price << ",0\n";
    }
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
        const Timestamp fill_date = fill.timestamp / 10U; // YYYYMMDD without the open/close phase digit
        out << fill_id << ','
            << fill.local_order_id << ','
            << fill.strategy_id << ','
            << fill_date << ','
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


std::string issueKind(ReconciliationIssueKind kind)
{
    return std::to_string(static_cast<int>(kind));
}

std::string accountingType(AccountingEventType type)
{
    return VenueContracts::V1::toString(type);
}

std::string ledgerKind(LedgerEntryKind kind)
{
    switch (kind) {
    case LedgerEntryKind::Fill: return "FILL";
    case LedgerEntryKind::TradingFee: return "TRADING_FEE";
    case LedgerEntryKind::Rebate: return "REBATE";
    case LedgerEntryKind::FundingPayment: return "FUNDING_PAYMENT";
    }
    return "UNKNOWN";
}

std::string sideText(Side side)
{
    return side == Side::Buy ? "BUY" : "SELL";
}

void atomicWrite(const std::filesystem::path& path, const std::string& data)
{
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("open temp dashboard state failed");
        out << data;
        out.flush();
        if (!out) throw std::runtime_error("write temp dashboard state failed");
    }
    std::filesystem::rename(tmp, path);
}

struct ReplayCheckpointMetadata {
    std::string mode;
    std::string profile;
    std::string start_date;
    std::string end_date;
    std::string checkpoint_date;
    int day_index = 0;
    int rows_processed = 0;
    std::uint64_t adapter_sequence = 0U;
    std::string economic_fingerprint;
    std::string stream_fingerprint;
    std::string chaos_fingerprint;
};

std::string checkpointMetadataText(const ReplayCheckpointMetadata& value)
{
    std::ostringstream out;
    out << "CANONICAL_REPLAY_CHECKPOINT_V1\n";
    out << "mode=" << value.mode << "\n";
    out << "profile=" << value.profile << "\n";
    out << "start=" << value.start_date << "\n";
    out << "end=" << value.end_date << "\n";
    out << "date=" << value.checkpoint_date << "\n";
    out << "day_index=" << value.day_index << "\n";
    out << "rows_processed=" << value.rows_processed << "\n";
    out << "adapter_sequence=" << value.adapter_sequence << "\n";
    out << "economic_fingerprint=" << value.economic_fingerprint << "\n";
    out << "stream_fingerprint=" << value.stream_fingerprint << "\n";
    out << "chaos_fingerprint=" << value.chaos_fingerprint << "\n";
    return out.str();
}

ReplayCheckpointMetadata readCheckpointMetadata(
    const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error(
            "resume checkpoint metadata is missing: " + path.string());

    std::string line;
    if (!std::getline(in, line) || line != "CANONICAL_REPLAY_CHECKPOINT_V1")
        throw std::runtime_error("unsupported replay checkpoint metadata");

    std::map<std::string, std::string> values;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        values.emplace(line.substr(0, pos), line.substr(pos + 1));
    }

    auto required = [&](const std::string& key) -> const std::string& {
        const auto it = values.find(key);
        if (it == values.end() || it->second.empty())
            throw std::runtime_error(
                "replay checkpoint metadata missing key: " + key);
        return it->second;
    };

    ReplayCheckpointMetadata result;
    result.mode = required("mode");
    result.profile = required("profile");
    result.start_date = required("start");
    result.end_date = required("end");
    result.checkpoint_date = required("date");
    result.day_index = std::stoi(required("day_index"));
    result.rows_processed = std::stoi(required("rows_processed"));
    result.adapter_sequence =
        static_cast<std::uint64_t>(std::stoull(required("adapter_sequence")));
    result.economic_fingerprint = required("economic_fingerprint");
    result.stream_fingerprint = required("stream_fingerprint");
    result.chaos_fingerprint = required("chaos_fingerprint");
    return result;
}

void writeCheckpointMetadata(
    const std::filesystem::path& path,
    const Args& args,
    const std::string& date,
    int day_index,
    int rows_processed,
    const MockExchange& adapter)
{
    ReplayCheckpointMetadata value;
    value.mode = args.mode;
    value.profile = args.profile;
    value.start_date = args.start_date;
    value.end_date = args.end_date;
    value.checkpoint_date = date;
    value.day_index = day_index;
    value.rows_processed = rows_processed;
    value.adapter_sequence =
        adapter.chaos().runtime().lastCheckpointSequence();
    value.economic_fingerprint =
        adapter.chaos().runtime().account().economicFingerprint();
    value.stream_fingerprint =
        adapter.chaos().runtime().streamFingerprint();
    value.chaos_fingerprint =
        adapter.chaos().evidenceFingerprint();
    atomicWrite(path, checkpointMetadataText(value));
}

void truncateResumeJournal(const std::filesystem::path& durable)
{
    const auto checkpoint = durable / "checkpoint_v1.bin";
    if (!std::filesystem::exists(checkpoint))
        throw std::runtime_error(
            "resume requires durable checkpoint_v1.bin");

    const auto journal = durable / "incremental_v1.bin";
    std::ofstream out(journal, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error(
            "unable to roll durable journal back to last safe checkpoint");
}

std::vector<MarketBarSnapshot> openPhaseBars(const DayData& day)
{
    auto bars = day.slice.bars;
    for (auto& b : bars) {
        b.bar.high = b.bar.open;
        b.bar.low = b.bar.open;
        b.bar.close = b.bar.open;
        b.bar.volume = 0.0;
    }
    return bars;
}

double cashTotal(const VenueContracts::V1::AccountSnapshot& account)
{
    for (const auto& b : account.balances)
        if (b.asset == "USD") return b.total;
    return account.equity;
}

double cashAvailable(const VenueContracts::V1::AccountSnapshot& account)
{
    for (const auto& b : account.balances)
        if (b.asset == "USD") return b.available;
    return account.equity - account.margin_used;
}

double quantityUnitsToDouble(const std::string& asset, std::int64_t units)
{
    const auto* entry = findByCanonicalAssetExact(asset);
    const auto* rules = entry == nullptr ? nullptr : rulesForExact(*entry);
    if (rules == nullptr) return 0.0;

    return signedScaledToDouble(units, rules->size_scale);
}

double moneyUnitsToDouble(std::int64_t units)
{
    return signedScaledToDouble(units, kMockMoneyScale);
}

double moneyUnitsToDoubleU(std::uint64_t units)
{
    return static_cast<double>(units) /
        static_cast<double>(kMockMoneyFactor);
}

// JSON number, or null when the value is not available (NaN).
std::string jsonNumber(double value)
{
    if (!std::isfinite(value))
        return "null";
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

std::string pipelineJson(const PipelineView& view)
{
    if (!view.available)
        return "null";
    std::ostringstream out;
    out << std::setprecision(15);
    out << "{\"date\":\"" << jsonEscape(view.date)
        << "\",\"decisionTimestamp\":" << view.decision_timestamp
        << ",\"liquidityLength\":" << LIQUIDITY_SPEC.length
        << ",\"liquidityTopN\":" << LIQUIDITY_TOP_N
        << ",\"totalCapital\":" << view.total_capital
        << ",\"approvedGross\":" << view.approved_gross
        << ",\"strategyCount\":" << view.strategy_count
        << ",\"observedAssets\":" << view.observed_assets
        << ",\"universeSize\":" << view.universe_size
        << ",\"activeSignals\":" << view.active_signals
        << ",\"cancelOrders\":" << view.cancel_orders
        << ",\"assets\":[";
    for (std::size_t i = 0; i < view.assets.size(); ++i) {
        const auto& a = view.assets[i];
        if (i) out << ',';
        out << "{\"asset\":\"" << jsonEscape(a.asset)
            << "\",\"close\":" << a.close
            << ",\"liquidity\":" << jsonNumber(a.liquidity)
            << ",\"liquidityRank\":" << a.liquidity_rank
            << ",\"inUniverse\":" << (a.in_universe ? "true" : "false")
            << ",\"strategies\":[";
        for (std::size_t j = 0; j < a.strategies.size(); ++j) {
            const auto& s = a.strategies[j];
            if (j) out << ',';
            out << "{\"strategyId\":" << s.strategy_id
                << ",\"name\":\"" << jsonEscape(s.name)
                << "\",\"capital\":" << s.capital
                << ",\"share\":" << s.share
                << ",\"signal\":" << s.signal
                << ",\"detail\":\"" << jsonEscape(s.detail)
                << "\",\"rule\":\"" << jsonEscape(s.rule)
                << "\",\"sizer\":\"" << jsonEscape(s.sizer)
                << "\",\"sizedWeight\":" << s.sized_weight
                << ",\"cappedWeight\":" << s.capped_weight
                << ",\"approvedWeight\":" << s.approved_weight
                << ",\"maxAsset\":" << s.max_asset
                << ",\"maxGross\":" << s.max_gross
                << ",\"grossAfterAssetCap\":" << s.gross_after_asset_cap
                << ",\"grossScale\":" << s.gross_scale
                << ",\"action\":\"" << s.action
                << "\"}";
        }
        out << "]"
            << ",\"signal\":" << a.signal
            << ",\"sizedWeight\":" << a.sized_weight
            << ",\"cappedWeight\":" << a.capped_weight
            << ",\"approvedWeight\":" << a.approved_weight
            << ",\"action\":\"" << a.action
            << "\",\"targetUsd\":" << (a.has_target ? jsonNumber(a.target_usd) : "null")
            << ",\"currentQty\":" << a.current_qty
            << ",\"currentUsd\":" << a.current_usd
            << ",\"orders\":[";
        for (std::size_t j = 0; j < a.orders.size(); ++j) {
            const auto& o = a.orders[j];
            if (j) out << ',';
            out << "{\"orderId\":\"" << o.order_id
                << "\",\"side\":\"" << (o.side == OrderSide::Buy ? "BUY" : "SELL")
                << "\",\"notionalUsd\":" << o.notional_usd
                << ",\"deltaUsd\":" << o.delta_notional_usd
                << ",\"pendingUsd\":" << o.pending_notional_usd
                << ",\"referenceClose\":" << o.reference_close
                << "}";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string dashboardStateJson(
    MockExchange& adapter,
    const Args& args,
    const std::string& phase,
    const std::string& date,
    Timestamp ts,
    int day_index,
    int day_count,
    int rows_processed,
    const std::vector<MarketBarSnapshot>& market,
    const std::vector<EquityPoint>& equity_history,
    const DashboardDecision& decision,
    std::uint64_t generation)
{
    const auto account =
        adapter.chaos().runtime().account().accountSnapshot(ts);

    MockReconciliation ledger_builder(
        adapter.chaos().runtime().account().config().fee_ppm);
    const auto local =
        ledger_builder.buildLocalExpected(
            adapter.chaos().runtime().stream());
    const auto venue_snapshot =
        adapter.chaos().runtime().stateSnapshot(
            ts,
            std::numeric_limits<std::size_t>::max());
    const auto report =
        ledger_builder.compare(local, venue_snapshot);

    const auto& orders =
        adapter.chaos().runtime().lifecycle().orders();
    const auto& stream =
        adapter.chaos().runtime().stream();

    std::vector<VenueContracts::V1::Fill> fills;
    std::vector<VenueContracts::V1::AccountingEvent> accounting;

    for (const auto& env : stream) {
        if (std::holds_alternative<
                VenueContracts::V1::Fill>(env.event))
            fills.push_back(
                std::get<VenueContracts::V1::Fill>(env.event));
        else if (std::holds_alternative<
                     VenueContracts::V1::AccountingEvent>(env.event))
            accounting.push_back(
                std::get<VenueContracts::V1::AccountingEvent>(env.event));
    }

    std::ostringstream out;
    out << std::setprecision(15);
    out << "{\n";
    out << "\"schemaVersion\":1,\n";
    out << "\"generation\":" << generation << ",\n";
    out << "\"phase\":\"" << jsonEscape(phase) << "\",\n";
    out << "\"historicalDate\":\"" << jsonEscape(date) << "\",\n";
    out << "\"businessTimestamp\":" << ts << ",\n";
    out << "\"dayIndex\":" << day_index << ",\n";
    out << "\"dayCount\":" << day_count << ",\n";
    out << "\"sourceRowsProcessed\":" << rows_processed << ",\n";
    out << "\"venueId\":\"MOCK\",\n";
    out << "\"environment\":\"MOCK\",\n";
    out << "\"replaySpeed\":" << args.speed << ",\n";
    out << "\"routeSafe\":"
        << (decision.route_safe ? "true" : "false") << ",\n";

    out << "\"reconciliation\":{\"state\":\""
        << reconciliationState(report.state)
        << "\",\"localSequence\":" << report.local_sequence
        << ",\"venueSequence\":" << report.venue_sequence
        << ",\"ledgerHeadHash\":\""
        << jsonEscape(report.ledger_head_hash)
        << "\",\"issues\":[";
    for (std::size_t i = 0; i < report.issues.size(); ++i) {
        if (i) out << ',';
        const auto& x = report.issues[i];
        out << "{\"kind\":\"" << issueKind(x.kind)
            << "\",\"asset\":\"" << jsonEscape(x.asset)
            << "\",\"orderId\":\"" << x.local_order_id
            << "\",\"localValue\":\"" << jsonEscape(x.local_value)
            << "\",\"venueValue\":\"" << jsonEscape(x.venue_value)
            << "\",\"message\":\"" << jsonEscape(x.message)
            << "\"}";
    }
    out << "]},\n";

    out << "\"account\":{\"equity\":" << account.equity
        << ",\"marginUsed\":" << account.margin_used
        << ",\"cashTotal\":" << cashTotal(account)
        << ",\"cashAvailable\":" << cashAvailable(account)
        << ",\"positions\":[";
    for (std::size_t i = 0; i < account.positions.size(); ++i) {
        if (i) out << ',';
        const auto& p = account.positions[i];
        out << "{\"asset\":\""
            << jsonEscape(p.instrument.market.canonical_asset)
            << "\",\"quantity\":" << p.signed_quantity
            << ",\"entryPrice\":" << p.entry_price
            << ",\"markPrice\":" << p.mark_price
            << ",\"unrealizedPnl\":" << p.unrealized_pnl
            << ",\"leverage\":" << p.leverage
            << "}";
    }
    out << "]},\n";

    out << "\"orders\":[";
    bool first = true;
    for (const auto& pair : orders) {
        if (!first) out << ',';
        first = false;
        const auto& o = pair.second;
        out << "{\"orderId\":\"" << pair.first
            << "\",\"strategyId\":" << o.intent.strategy_id
            << ",\"asset\":\""
            << jsonEscape(o.intent.instrument.market.canonical_asset)
            << "\",\"side\":\"" << sideText(o.intent.side)
            << "\",\"quantity\":" << o.intent.quantity
            << ",\"filledQuantity\":" << o.cumulative_filled_quantity
            << ",\"remainingQuantity\":" << o.remaining_quantity
            << ",\"limitPrice\":" << o.intent.limit_price
            << ",\"status\":\""
            << VenueContracts::V1::toString(o.status)
            << "\",\"nativeOrderId\":\""
            << jsonEscape(o.native_references.native_order_id)
            << "\",\"createdAt\":" << o.intent.created_at
            << ",\"activeFrom\":" << o.intent.active_from
            << "}";
    }
    out << "],\n";

    out << "\"fills\":[";
    for (std::size_t i = 0; i < fills.size(); ++i) {
        if (i) out << ',';
        const auto& f = fills[i];
        out << "{\"fillId\":\""
            << jsonEscape(f.native_references.native_fill_id)
            << "\",\"orderId\":\"" << f.local_order_id
            << "\",\"strategyId\":" << f.strategy_id
            << ",\"timestamp\":" << f.timestamp
            << ",\"asset\":\""
            << jsonEscape(f.instrument.market.canonical_asset)
            << "\",\"side\":\"" << sideText(f.side)
            << "\",\"quantity\":" << f.quantity
            << ",\"price\":" << f.price
            << "}";
    }
    out << "],\n";

    out << "\"accounting\":[";
    for (std::size_t i = 0; i < accounting.size(); ++i) {
        if (i) out << ',';
        const auto& e = accounting[i];
        double amount = 0.0;
        try { amount = std::stod(e.amount); } catch (...) { amount = 0.0; }
        out << "{\"correlationId\":\""
            << jsonEscape(e.correlation_id)
            << "\",\"timestamp\":" << e.timestamp
            << ",\"type\":\"" << accountingType(e.type)
            << "\",\"amount\":" << amount
            << ",\"asset\":\""
            << jsonEscape(
                   e.market ? e.market->canonical_asset : std::string{})
            << "\",\"nativeFillId\":\""
            << jsonEscape(e.native_references.native_fill_id)
            << "\"}";
    }
    out << "],\n";

    std::vector<MarketBarSnapshot> sorted_market = market;
    std::sort(
        sorted_market.begin(),
        sorted_market.end(),
        [](const auto& a, const auto& b) {
            return a.coin < b.coin;
        });

    out << "\"market\":[";
    for (std::size_t i = 0; i < sorted_market.size(); ++i) {
        if (i) out << ',';
        const auto& b = sorted_market[i];
        out << "{\"asset\":\"" << jsonEscape(b.coin)
            << "\",\"open\":" << b.bar.open
            << ",\"high\":" << b.bar.high
            << ",\"low\":" << b.bar.low
            << ",\"close\":" << b.bar.close
            << ",\"volume\":" << b.bar.volume
            << "}";
    }
    out << "],\n";

    out << "\"equityHistory\":[";
    for (std::size_t i = 0; i < equity_history.size(); ++i) {
        if (i) out << ',';
        out << "{\"label\":\""
            << jsonEscape(equity_history[i].label)
            << "\",\"equity\":" << equity_history[i].equity
            << ",\"balance\":" << equity_history[i].balance
            << ",\"gross\":" << equity_history[i].gross
            << ",\"closeLoop\":\"" << equity_history[i].close_loop
            << "\"}";
    }
    out << "],\n";

    // Latest approved target exposure per asset (USD) from the last close decision.
    out << "\"targets\":[";
    bool first_target = true;
    for (const auto& [asset, usd] : decision.targets) {
        if (!first_target) out << ',';
        first_target = false;
        out << "{\"asset\":\"" << jsonEscape(asset) << "\",\"usd\":" << usd << "}";
    }
    out << "],\n";

    // Stage-by-stage lineage of the latest close decision (Pipeline page).
    out << "\"pipeline\":" << pipelineJson(decision.pipeline) << ",\n";

    out << "\"ledger\":{\"valid\":"
        << (local.ledger.valid ? "true" : "false")
        << ",\"error\":\"" << jsonEscape(local.ledger.error)
        << "\",\"headHash\":\""
        << jsonEscape(local.ledger.head_hash)
        << "\",\"entries\":[";
    for (std::size_t i = 0; i < local.ledger.entries.size(); ++i) {
        if (i) out << ',';
        const auto& e = local.ledger.entries[i];
        std::string side;
        if (e.kind == LedgerEntryKind::Fill)
            side = e.position_delta_units >= 0 ? "BUY" : "SELL";
        out << "{\"sequence\":" << e.stream_sequence
            << ",\"kind\":\"" << ledgerKind(e.kind)
            << "\",\"entryId\":\"" << jsonEscape(e.entry_id)
            << "\",\"eventTime\":" << e.event_time
            << ",\"asset\":\"" << jsonEscape(e.asset)
            << "\",\"orderId\":\"" << e.local_order_id
            << "\",\"strategyId\":" << e.strategy_id
            << ",\"nativeFillId\":\""
            << jsonEscape(e.native_fill_id)
            << "\",\"side\":\"" << side
            << "\",\"positionDelta\":"
            << quantityUnitsToDouble(e.asset, e.position_delta_units)
            << ",\"cashDelta\":"
            << moneyUnitsToDouble(e.cash_delta_units)
            << ",\"realizedPnl\":"
            << moneyUnitsToDouble(e.realized_pnl_units)
            << ",\"grossNotional\":"
            << moneyUnitsToDoubleU(e.gross_notional_units)
            << ",\"entryHash\":\""
            << jsonEscape(e.entry_hash)
            << "\"}";
    }
    out << "]},\n";

    // Assets a manual target may use now: priced in this phase and explicitly MOCK-routable.
    out << "\"routableAssets\":[";
    bool first_routable = true;
    for (const auto& bar : sorted_market) {
        const auto* entry = findByCanonicalAssetExact(bar.coin);
        const auto* rules = entry == nullptr ? nullptr : rulesForExact(*entry);
        if (entry == nullptr || !entry->enabled || rules == nullptr || !rules->valid())
            continue;
        if (!first_routable) out << ',';
        first_routable = false;
        out << "\"" << jsonEscape(bar.coin) << "\"";
    }
    out << "],\n";

    const ManualControlView& manual = decision.manual;
    out << "\"manual\":{\"ready\":" << (manual.accepting ? "true" : "false")
        << ",\"blocker\":\"" << (manual.accepting ? "" : "REPLAY_NOT_RUNNING")
        << "\",\"maxGrossLeverage\":" << jsonNumber(manual.max_gross_leverage)
        << ",\"maxAssetWeight\":" << jsonNumber(manual.max_asset_weight)
        << ",\"resyncPending\":" << (manual.resync_pending ? "true" : "false")
        << ",\"lastExecuted\":\"" << jsonEscape(manual.last_executed)
        << "\",\"lastOrderIds\":\"" << jsonEscape(manual.last_order_ids)
        << "\",\"weights\":[";
    bool first_weight = true;
    for (const auto& [asset, weight] : manual.weights) {
        if (!first_weight) out << ',';
        first_weight = false;
        out << "{\"asset\":\"" << jsonEscape(asset) << "\",\"weight\":" << jsonNumber(weight) << "}";
    }
    out << "],\"decisionTimestamp\":0,\"executionTimestamp\":0"
        << ",\"lastCorrelationId\":\"" << jsonEscape(manual.last_request_id)
        << "\",\"lastActor\":\"" << jsonEscape(manual.last_actor)
        << "\",\"lastStatus\":\"" << jsonEscape(manual.last_status)
        << "\",\"lastDetail\":\"" << jsonEscape(manual.last_detail) << "\"},\n";

    out << "\"economicFingerprint\":\""
        << jsonEscape(
               adapter.chaos().runtime().account().economicFingerprint())
        << "\",\n";
    out << "\"streamFingerprint\":\""
        << jsonEscape(adapter.chaos().runtime().streamFingerprint())
        << "\",\n";
    out << "\"step56RuntimeFingerprint\":\""
        << kStep56RuntimeFingerprint << "\",\n";
    out << "\"step57ManualFingerprint\":\""
        << "65c0a7b418d7f3873f38f6a1daa87d5d915e4a254452865b79a69df24c3dc5f0"
        << "\"\n";
    out << "}\n";
    return out.str();
}

// Day point for the dashboard charts: equity, balance (equity minus unrealized PnL),
// gross position value (for leverage) and whether local and venue state reconcile
// (the "close loop") at that time.
EquityPoint dashboardDayPoint(MockExchange& adapter, const std::string& date, Timestamp ts)
{
    const auto account =
        adapter.chaos().runtime().account().accountSnapshot(ts);
    double unrealized = 0.0;
    double gross = 0.0;
    for (const auto& position : account.positions) {
        unrealized += position.unrealized_pnl;
        gross += std::abs(position.signed_quantity * position.mark_price);
    }

    MockReconciliation ledger_builder(
        adapter.chaos().runtime().account().config().fee_ppm);
    const auto report = ledger_builder.compare(
        ledger_builder.buildLocalExpected(adapter.chaos().runtime().stream()),
        adapter.chaos().runtime().stateSnapshot(ts, std::numeric_limits<std::size_t>::max()));

    return {date, account.equity, account.equity - unrealized, gross, reconciliationState(report.state)};
}

void emitDashboardState(
    MockExchange& adapter,
    const Args& args,
    const std::string& phase,
    const std::string& date,
    Timestamp ts,
    int day_index,
    int day_count,
    int rows_processed,
    const std::vector<MarketBarSnapshot>& market,
    std::vector<EquityPoint>& history,
    const DashboardDecision& decision,
    std::uint64_t& generation,
    bool append_equity)
{
    ++generation;

    if (append_equity) {
        history.push_back(dashboardDayPoint(adapter, date, ts));
        if (history.size() > MAX_DASHBOARD_EQUITY_POINTS)
            history.erase(history.begin());
    }

    atomicWrite(
        args.dashboard_state_dir / "state.json",
        dashboardStateJson(
            adapter, args, phase, date, ts,
            day_index, day_count, rows_processed,
            market, history, decision, generation));

    LG_DEBUG("Dashboard state {} published: phase {}, date {}", generation, phase, date);
    LG_DEBUG("Dashboard state {} timestamp {}", generation, ts);

    if (args.ui_delay_ms > 0)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(args.ui_delay_ms));
}

// One confirmed Manual Control request, written by dashboard-api:
//   REPLAY_MANUAL_V1
//   request_id=... / actor=... / request_hash=... / action=SET
//   target=ASSET,FRACTION   (one line per asset; the rest stays in cash)
struct ManualRequest {
    std::string request_id;
    std::string actor;
    std::string request_hash;
    std::map<std::string, double> weights;
};

ManualRequest readManualRequest(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("unable to open manual request");
    std::string line;
    if (!std::getline(in, line) || line != "REPLAY_MANUAL_V1")
        throw std::runtime_error("unsupported manual request header");

    ManualRequest request;
    std::string action;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("malformed manual request line");
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "request_id") request.request_id = value;
        else if (key == "actor") request.actor = value;
        else if (key == "request_hash") request.request_hash = value;
        else if (key == "action") action = value;
        else if (key == "target") {
            const auto comma = value.find(',');
            if (comma == std::string::npos)
                throw std::runtime_error("malformed manual target line");
            const std::string asset = value.substr(0, comma);
            if (!request.weights.emplace(asset, std::stod(value.substr(comma + 1))).second)
                throw std::runtime_error("duplicate manual target asset " + asset);
        } else
            throw std::runtime_error("unknown manual request field " + key);
    }
    if (request.request_id.empty())
        throw std::runtime_error("manual request has no request_id");
    if (action != "SET")
        throw std::runtime_error("manual request action must be SET");
    return request;
}

// Record the outcome in the request file and move it to processed/ (the audit log).
void finishManualRequest(
    const std::filesystem::path& path,
    const std::filesystem::path& processed_dir,
    const ManualControlView& view,
    const std::string& date)
{
    {
        std::ofstream outcome(path, std::ios::app);
        outcome << "result=" << view.last_status << "\n"
                << "detail=" << view.last_detail << "\n"
                << "replay_date=" << date << "\n"
                << "orders=" << view.last_order_ids << "\n";
    }
    std::filesystem::rename(path, processed_dir / path.filename());
}

// Execute the newest queued manual request at `execution_timestamp` and `prices`
// (the open of `day` for requests that arrived overnight, a point before its close for
// requests that arrived during the day). Older queued requests are superseded: only
// the operator's latest portfolio matters. Returns true when orders were routed, so
// the caller matches them at the same prices.
bool executeManualRequests(
    const std::filesystem::path& state_dir,
    const DayData& day,
    const std::string& moment,                 // "open" or "close", for the record
    Timestamp execution_timestamp,
    const ExecutionReferencePrices& prices,
    ReplayRuntime& replay,
    ManualControlView& view)
{
    const auto requests_dir = state_dir / "requests";
    const auto processed_dir = state_dir / "processed";
    if (!std::filesystem::exists(requests_dir))
        return false;

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(requests_dir))
        if (entry.is_regular_file() && entry.path().extension() == ".request")
            files.emplace_back(entry.last_write_time(), entry.path());
    if (files.empty())
        return false;
    std::sort(files.begin(), files.end());
    std::filesystem::create_directories(processed_dir);

    for (std::size_t i = 0; i + 1 < files.size(); ++i) {
        view.last_request_id = files[i].second.stem().string();
        view.last_status = "SUPERSEDED";
        view.last_detail = "A newer manual request replaced it before execution";
        view.last_order_ids.clear();
        finishManualRequest(files[i].second, processed_dir, view, day.date);
    }

    const auto& path = files.back().second;
    ManualRequest request;
    std::string rejection;
    ReplayRuntime::ManualRebalanceResult result;
    const OrderID first_order = replay.nextOrderId();
    try {
        request = readManualRequest(path);
        for (const auto& [asset, weight] : request.weights) {
            (void)weight;
            const auto* entry = findByCanonicalAssetExact(asset);
            const auto* rules = entry == nullptr ? nullptr : rulesForExact(*entry);
            if (entry == nullptr || !entry->enabled || rules == nullptr || !rules->valid())
                throw std::runtime_error(asset + " has no explicit MOCK route");
        }
        result = replay.rebalanceToManualTarget(request.weights, execution_timestamp, prices);
    } catch (const std::exception& error) {
        rejection = error.what();
    }

    view.last_request_id = request.request_id.empty() ? path.stem().string() : request.request_id;
    view.last_actor = request.actor;
    view.last_order_ids.clear();
    if (!rejection.empty()) {
        view.last_status = "REJECTED";
        view.last_detail = rejection;
        LG_WARN("Manual request {} rejected: {}", view.last_request_id, rejection);
    } else {
        view.weights = request.weights;
        view.resync_pending = replay.targetResyncPending();
        view.last_executed = day.date + " " + moment;
        view.last_status = "EXECUTED";
        view.last_detail = std::to_string(result.submits) + " orders at the " + day.date + " " + moment +
            (result.discarded_strategy_plan ? " (replaced the pending strategy plan)" : "") +
            "; strategies re-sync at the " + day.date + " close";
        if (result.submits > 0U)
            view.last_order_ids = std::to_string(first_order) + "-" + std::to_string(replay.nextOrderId() - 1U);
        LG_INFO("Manual request {} by {} executed at the {} {}: {} orders",
                request.request_id, request.actor, day.date, moment, result.submits);
    }
    finishManualRequest(path, processed_dir, view, day.date);
    return rejection.empty() && result.submits > 0U;
}

// Fill manual orders at exactly `prices` (one flat synthetic bar per asset at
// `timestamp`); daily data has no intraday path, so these are the execution prices.
void matchManualAtPrices(
    MockExchange& adapter,
    const DayData& day,
    Timestamp timestamp,
    const ExecutionReferencePrices& prices)
{
    for (const auto& value : day.slice.bars) {
        if (!prices.contains(value.coin))
            continue;
        MarketBarObservation observation;
        observation.canonical_asset = value.coin;
        observation.event_time = timestamp;
        const double price = prices.get(value.coin);
        observation.bar.open = price;
        observation.bar.high = price;
        observation.bar.low = price;
        observation.bar.close = price;
        observation.bar.volume = value.bar.volume;
        const auto result = adapter.processMarketBar(observation);
        if (result.status == MockVenue::ChaosStatus::VenueUnavailable ||
            result.status == MockVenue::ChaosStatus::OutOfOrderRejected)
            throw std::runtime_error("manual rebalance matching was rejected");
    }
}

bool inRange(
    const std::string& date,
    const std::string& start,
    const std::string& end)
{
    return !start.empty() &&
        date >= start &&
        date <= end;
}


} // namespace

int main(int argc, char** argv)
{
    Logger::Instance().Setup(false, false);

    try {
        const Args args = parseArgs(argc, argv);
        const auto mapping = loadMapping(args.mapping);

        std::size_t source_rows = 0U;
        std::set<std::string> window_symbols;
        auto days = loadDays(
            args,
            mapping,
            &source_rows,
            &window_symbols);

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

        MatchingFillConfig matching;
        MockAccountingConfig accounting;
        if (args.profile == "realtest-parity") {
            matching.ignore_volume_capacity = true;
            matching.max_adverse_slippage_ppm = 0U;
            matching.fee_ppm = 0U;
            accounting.fee_ppm = 0U;
        }

        std::error_code ignored;
        std::optional<ReplayCheckpointMetadata> resume_checkpoint;
        if (args.resume) {
            resume_checkpoint =
                readCheckpointMetadata(args.checkpoint_state);
            if (resume_checkpoint->mode != args.mode ||
                resume_checkpoint->profile != args.profile ||
                resume_checkpoint->start_date != args.start_date ||
                resume_checkpoint->end_date != args.end_date)
                throw std::runtime_error(
                    "resume checkpoint does not match selected replay campaign");
            truncateResumeJournal(args.durable);
        } else {
            std::filesystem::remove_all(args.durable, ignored);
            std::filesystem::create_directories(args.durable);
            if (!args.checkpoint_state.empty()) {
                std::filesystem::remove(args.checkpoint_state, ignored);
                std::filesystem::remove(
                    args.checkpoint_state.string() + ".tmp", ignored);
            }
        }

        if (args.mode == "dashboard") {
            std::filesystem::create_directories(
                args.dashboard_state_dir);
            std::filesystem::remove(
                args.dashboard_state_dir / "state.json",
                ignored);
            std::filesystem::remove(
                args.dashboard_state_dir / "state.json.tmp",
                ignored);
            // Manual requests belong to one run; never apply a previous run's queue.
            std::filesystem::remove_all(args.dashboard_state_dir / "requests", ignored);
            std::filesystem::remove_all(args.dashboard_state_dir / "processed", ignored);
            std::filesystem::create_directories(args.dashboard_state_dir / "requests");
        }

        MockExchange adapter(
            args.durable,
            chaos,
            matching,
            accounting);

        ReplayRuntime replay(
            strategy,
            risk,
            planner,
            {1U},
            adapter,
            args.profile == "realtest-parity",
            args.profile == "realtest-parity");

        std::unique_ptr<TimeHandler> release_clock;
        std::vector<EquityPoint> equity_history;
        // Dashboard only: the latest close decision (kept until the next one).
        DashboardDecision dashboard_decision;
        if (args.mode == "dashboard") {
            replay.enableDecisionDiagnostics();
            dashboard_decision.manual.accepting = true;
            const auto limits = replay.manualLimits();
            dashboard_decision.manual.max_gross_leverage = limits.max_gross_leverage;
            dashboard_decision.manual.max_asset_weight = limits.max_asset_weight;
        }
        std::uint64_t generation = 0U;
        int day_index = 0;
        int rows_processed = 0;
        std::string resume_after_date;

        if (resume_checkpoint.has_value()) {
            if (args.profile != "realtest-parity")
                throw std::runtime_error(
                    "resume state rebuild currently requires realtest-parity");

            const auto& checkpoint = *resume_checkpoint;
            if (adapter.chaos().runtime().lastSequence() !=
                    checkpoint.adapter_sequence ||
                adapter.chaos().runtime().lastCheckpointSequence() !=
                    checkpoint.adapter_sequence ||
                adapter.chaos().runtime().account().economicFingerprint() !=
                    checkpoint.economic_fingerprint ||
                adapter.chaos().runtime().streamFingerprint() !=
                    checkpoint.stream_fingerprint)
                throw std::runtime_error(
                    "durable venue checkpoint does not match replay checkpoint metadata");

            const auto scratch_dir =
                args.durable.parent_path() /
                (args.durable.filename().string() + ".resume_rebuild_tmp");
            std::filesystem::remove_all(scratch_dir, ignored);
            std::filesystem::create_directories(scratch_dir);

            StrategySignalEngine scratch_strategy = makeStrategyEngine();
            PortfolioRiskEngine scratch_risk = makeRiskEngine();
            NotionalOrderPlanner scratch_planner;
            MockExchange scratch_adapter(
                scratch_dir, chaos, matching, accounting);
            ReplayRuntime scratch_replay(
                scratch_strategy,
                scratch_risk,
                scratch_planner,
                {1U},
                scratch_adapter,
                true,
                true);

            int scratch_day_index = 0;
            int scratch_rows = 0;
            bool found_checkpoint_date = false;
            std::vector<EquityPoint> scratch_equity_history;
            std::uint64_t scratch_generation = 0U;
            for (const auto& scratch_pair : days) {
                const DayData& scratch_day = scratch_pair.second;
                if (scratch_day.date > checkpoint.checkpoint_date)
                    break;
                ++scratch_day_index;
                scratch_replay.onExecutionOpen(
                    scratch_day.open_timestamp,
                    scratch_day.open_prices);
                matchRealTestParityAtExecutionOpen(
                    scratch_adapter,
                    scratch_day);

                if (args.mode == "dashboard" &&
                    inRange(
                        scratch_day.date,
                        args.visual_start,
                        args.visual_end))
                    ++scratch_generation;

                scratch_replay.onClosedSlice(scratch_day.slice);
                scratch_rows +=
                    static_cast<int>(scratch_day.slice.bars.size());

                if (args.mode == "dashboard" &&
                    inRange(
                        scratch_day.date,
                        args.visual_start,
                        args.visual_end)) {
                    ++scratch_generation;
                    scratch_equity_history.push_back(dashboardDayPoint(
                        scratch_adapter, scratch_day.date, scratch_day.close_timestamp));
                    if (scratch_equity_history.size() > MAX_DASHBOARD_EQUITY_POINTS)
                        scratch_equity_history.erase(
                            scratch_equity_history.begin());
                }

                if (scratch_day.date == checkpoint.checkpoint_date)
                    found_checkpoint_date = true;
            }

            if (!found_checkpoint_date ||
                scratch_day_index != checkpoint.day_index ||
                scratch_rows != checkpoint.rows_processed)
                throw std::runtime_error(
                    "replay checkpoint date/index no longer matches historical input");

            if (scratch_adapter.chaos().runtime().lastSequence() !=
                    checkpoint.adapter_sequence ||
                scratch_adapter.chaos().runtime().account().economicFingerprint() !=
                    checkpoint.economic_fingerprint ||
                scratch_adapter.chaos().runtime().streamFingerprint() !=
                    checkpoint.stream_fingerprint ||
                scratch_adapter.chaos().evidenceFingerprint() !=
                    checkpoint.chaos_fingerprint)
                throw std::runtime_error(
                    "deterministic in-memory rebuild diverged from durable venue checkpoint");

            adapter.chaos().restoreResumeState(
                scratch_adapter.chaos().snapshotResumeState());
            replay.restoreResumeState(
                scratch_replay.snapshotResumeState());
            if (args.mode == "dashboard") {
                equity_history = std::move(scratch_equity_history);
                generation = scratch_generation;
            }
            day_index = checkpoint.day_index;
            rows_processed = checkpoint.rows_processed;
            resume_after_date = checkpoint.checkpoint_date;
            std::filesystem::remove_all(scratch_dir, ignored);

            LG_INFO("Resuming replay after checkpoint {} (day {})",
                    checkpoint.checkpoint_date, checkpoint.day_index);
            LG_DEBUG("Resume adapter sequence {}", checkpoint.adapter_sequence);
        }

        for (const auto& pair : days) {
            const DayData& day = pair.second;
            if (!resume_after_date.empty() && day.date <= resume_after_date)
                continue;
            ++day_index;

            const bool paced =
                inRange(
                    day.date,
                    args.pace_start,
                    args.pace_end);

            if (paced && !release_clock) {
                const auto real_reference =
                    TimeHandler::Clock::now();
                const auto bias =
                    day.open_time - real_reference;
                release_clock =
                    std::make_unique<TimeHandler>(
                        args.speed,
                        bias,
                        real_reference);
            }

            if (paced)
                release_clock->sleepUntil(
                    day.open_time);

            // A manual request that arrived overnight replaces the pending strategy plan
            // and executes at this open instead.
            const bool manual_at_open =
                args.mode == "dashboard" &&
                executeManualRequests(
                    args.dashboard_state_dir,
                    day,
                    "open",
                    day.open_timestamp,
                    day.open_prices,
                    replay,
                    dashboard_decision.manual);
            if (!manual_at_open)
                replay.onExecutionOpen(
                    day.open_timestamp,
                    day.open_prices);

            if (args.profile == "realtest-parity")
                matchRealTestParityAtExecutionOpen(
                    adapter,
                    day);

            if (args.mode == "dashboard" &&
                inRange(
                    day.date,
                    args.visual_start,
                    args.visual_end)) {
                dashboard_decision.route_safe = replay.routeSafe();
                emitDashboardState(
                    adapter,
                    args,
                    "REPLAY_OPEN",
                    day.date,
                    day.open_timestamp,
                    day_index,
                    static_cast<int>(days.size()),
                    rows_processed,
                    openPhaseBars(day),
                    equity_history,
                    dashboard_decision,
                    generation,
                    false);
            }

            if (paced)
                release_clock->sleepUntil(
                    day.close_time);

            // A manual request that arrived during the day executes at the first price
            // after it, the close, just before the end-of-day decision (which then
            // re-syncs to the strategy targets for the next open).
            if (args.mode == "dashboard") {
                ExecutionReferencePrices closes;
                for (const auto& bar : day.slice.bars)
                    closes.set(bar.coin, bar.bar.close);
                const Timestamp manual_timestamp = day.close_timestamp - 4U; // YYYYMMDD5: between open and close
                if (executeManualRequests(
                        args.dashboard_state_dir,
                        day,
                        "close",
                        manual_timestamp,
                        closes,
                        replay,
                        dashboard_decision.manual))
                    matchManualAtPrices(adapter, day, manual_timestamp, closes);
            }

            replay.onClosedSlice(day.slice);
            dashboard_decision.manual.resync_pending = replay.targetResyncPending();
            if (args.mode == "dashboard" && replay.hasPendingPlan()) {
                const auto& exposures = replay.pendingPlan().global_target.values();
                dashboard_decision.targets = std::map<std::string, double>(exposures.begin(), exposures.end());
                dashboard_decision.pipeline = dashboardPipeline(replay, strategy, day.slice, day.date);
            }
            rows_processed +=
                static_cast<int>(
                    day.slice.bars.size());

            if (args.mode == "dashboard" &&
                inRange(
                    day.date,
                    args.visual_start,
                    args.visual_end)) {
                dashboard_decision.route_safe = replay.routeSafe();
                emitDashboardState(
                    adapter,
                    args,
                    "REPLAY_CLOSE",
                    day.date,
                    day.close_timestamp,
                    day_index,
                    static_cast<int>(days.size()),
                    rows_processed,
                    day.slice.bars,
                    equity_history,
                    dashboard_decision,
                    generation,
                    true);
            }

            const bool stop_here =
                args.stop_after_days > 0 &&
                day_index == args.stop_after_days;
            const bool periodic_checkpoint =
                args.checkpoint_every > 0 &&
                (day_index % args.checkpoint_every) == 0;

            if (stop_here || periodic_checkpoint) {
                if (!adapter.chaos().runtime().checkpoint())
                    throw std::runtime_error(
                        "unable to persist safe day-boundary MOCK checkpoint");
                writeCheckpointMetadata(
                    args.checkpoint_state,
                    args,
                    day.date,
                    day_index,
                    rows_processed,
                    adapter);
            }

            if (stop_here) {
                std::filesystem::create_directories(
                    args.output.parent_path());
                std::ofstream stopped(args.output);
                if (!stopped)
                    throw std::runtime_error(
                        "unable to open checkpointed replay summary");
                stopped << "{\n"
                    << "  \"result\": \"CHECKPOINTED\",\n"
                    << "  \"mode\": \"" << jsonEscape(args.mode) << "\",\n"
                    << "  \"profile\": \"" << jsonEscape(args.profile) << "\",\n"
                    << "  \"checkpointDate\": \"" << jsonEscape(day.date) << "\",\n"
                    << "  \"daysProcessed\": " << day_index << ",\n"
                    << "  \"rowsProcessed\": " << rows_processed << ",\n"
                    << "  \"adapterSequence\": "
                    << adapter.chaos().runtime().lastCheckpointSequence() << "\n"
                    << "}\n";
                LG_INFO("Saved replay checkpoint at {} (day {}) to {}",
                        day.date, day_index, args.checkpoint_state.string());
                return 0;
            }
        }

        const DayData& last =
            days.rbegin()->second;
        const Timestamp final_timestamp =
            last.close_timestamp;

        const auto evidence =
            replay.finalizeEvidence(
                final_timestamp);
        const auto account =
            adapter.chaos().runtime().
                account().accountSnapshot(
                    final_timestamp);
        const auto open_orders =
            adapter.chaos().runtime().
                account().openOrdersSnapshot(
                    adapter.chaos().runtime().
                        lifecycle(),
                    final_timestamp);
        const auto& reconciliation =
            adapter.chaos().lastReconciliation();
        const auto margin =
            adapter.chaos().runtime().
                account().marginState(
                    final_timestamp);

        if (args.profile == "realtest-parity") {
            exportParityFillsForRealTest(
                args.fills_output,
                replay.parityFills(),
                mapping);
        } else {
            exportFillsForRealTest(
                args.fills_output,
                adapter.chaos().runtime().stream(),
                mapping);
        }

        const bool pass =
            reconciliation.clean() &&
            replay.routeSafe() &&
            evidence.closed_slices ==
                days.size() &&
            evidence.signal_batches ==
                days.size() &&
            evidence.decision_batches ==
                days.size() &&
            evidence.planning_batches ==
                days.size() &&
            // A zero-fee profile legitimately emits no TradingFee accounting
            // events. Fee-bearing MOCK profiles keep one fee event per fill.
            evidence.canonical_accounting_events ==
                (accounting.fee_ppm == 0U
                    ? 0U
                    : evidence.canonical_fills) &&
            !evidence.full_run_fingerprint.empty();

        if (args.mode == "dashboard") {
            // The replay stops here: no further manual requests are applied.
            dashboard_decision.manual.accepting = false;
            dashboard_decision.route_safe = replay.routeSafe();
            emitDashboardState(
                adapter,
                args,
                pass ? "REPLAY_COMPLETE" : "ERROR",
                last.date,
                final_timestamp,
                day_index,
                static_cast<int>(days.size()),
                rows_processed,
                last.slice.bars,
                equity_history,
                dashboard_decision,
                generation,
                false);
        }

        std::filesystem::create_directories(
            args.output.parent_path());

        std::ofstream out(args.output);
        if (!out)
            throw std::runtime_error(
                "unable to open output summary: " +
                args.output.string());

        out << std::setprecision(15);
        out << "{\n";
        out << "  \"result\": \""
            << (pass ? "PASS" : "FAIL")
            << "\",\n";
        out << "  \"campaign\": "
               "\"CANONICAL_RESEARCH_REPLAY_V1\",\n";
        out << "  \"mode\": \""
            << jsonEscape(args.mode) << "\",\n";
        out << "  \"profile\": \""
            << jsonEscape(args.profile) << "\",\n";
        out << "  \"releaseMode\": \""
            << (args.pace_start.empty()
                ? "UNPACED_BUSINESS_TIME"
                : "TIMEHANDLER_PACED_SEGMENT")
            << "\",\n";
        out << "  \"paceStart\": \""
            << jsonEscape(args.pace_start)
            << "\",\n";
        out << "  \"paceEnd\": \""
            << jsonEscape(args.pace_end)
            << "\",\n";
        out << "  \"visualStart\": \""
            << jsonEscape(args.visual_start)
            << "\",\n";
        out << "  \"visualEnd\": \""
            << jsonEscape(args.visual_end)
            << "\",\n";
        out << "  \"firstDate\": \""
            << jsonEscape(days.begin()->first)
            << "\",\n";
        out << "  \"lastDate\": \""
            << jsonEscape(days.rbegin()->first)
            << "\",\n";
        out << "  \"sourceRows\": "
            << source_rows << ",\n";
        out << "  \"days\": "
            << days.size() << ",\n";
        out << "  \"sourceSymbolsInWindow\": "
            << window_symbols.size() << ",\n";
        out << "  \"mappingEntries\": "
            << mapping.size() << ",\n";
        out << "  \"timeHandlerSpeed\": "
            << args.speed << ",\n";
        out << "  \"uiDelayMs\": "
            << args.ui_delay_ms << ",\n";
        out << "  \"volumeTransform\": \"IDENTITY\",\n";
        out << "  \"matcherIgnoresVolumeCapacity\": "
            << (matching.ignore_volume_capacity ? "true" : "false")
            << ",\n";
        out << "  \"matcherParticipationPpm\": "
            << matching.participation_ppm << ",\n";
        out << "  \"matcherMaxAdverseSlippagePpm\": "
            << matching.max_adverse_slippage_ppm
            << ",\n";
        out << "  \"strategy\": \"Pure_RSI\",\n";
        out << "  \"plannedSubmits\": "
            << evidence.planned_submits << ",\n";
        out << "  \"plannedCancels\": "
            << evidence.planned_cancels << ",\n";
        out << "  \"canonicalFills\": "
            << evidence.canonical_fills << ",\n";
        out << "  \"canonicalAccountingEvents\": "
            << evidence.canonical_accounting_events
            << ",\n";
        out << "  \"knownOrders\": "
            << adapter.chaos().runtime().
                lifecycle().orders().size()
            << ",\n";
        out << "  \"openOrders\": "
            << open_orders.orders.size()
            << ",\n";
        out << "  \"finalPositions\": "
            << account.positions.size()
            << ",\n";
        out << "  \"finalEquity\": "
            << account.equity << ",\n";
        out << "  \"settledCash\": "
            << margin.settled_cash << ",\n";
        out << "  \"realizedPnl\": "
            << margin.realized_pnl << ",\n";
        out << "  \"unrealizedPnl\": "
            << margin.unrealized_pnl << ",\n";
        out << "  \"grossExposure\": "
            << margin.gross_exposure << ",\n";
        out << "  \"feesPaid\": "
            << signedScaledToDouble(
                   adapter.chaos().runtime().
                       account().feesPaidUnits(),
                   kMockMoneyScale)
            << ",\n";
        out << "  \"reconciliation\": \""
            << reconciliationState(
                   reconciliation.state)
            << "\",\n";
        out << "  \"reconciliationIssues\": "
            << reconciliation.issues.size()
            << ",\n";
        out << "  \"routeSafe\": "
            << (replay.routeSafe()
                ? "true" : "false")
            << ",\n";
        out << "  \"economicFingerprint\": \""
            << evidence.economic_fingerprint
            << "\",\n";
        out << "  \"streamFingerprint\": \""
            << evidence.stream_fingerprint
            << "\",\n";
        out << "  \"ledgerHeadHash\": \""
            << evidence.ledger_head_hash
            << "\",\n";
        out << "  \"chaosEvidenceFingerprint\": \""
            << evidence.chaos_evidence_fingerprint
            << "\",\n";
        out << "  \"fullRunFingerprint\": \""
            << evidence.full_run_fingerprint
            << "\",\n";
        out << "  \"step56RuntimeFingerprint\": \""
            << evidence.step56_runtime_fingerprint
            << "\"\n";
        out << "}\n";
        out.close();

        LG_INFO("Replay {} ({} mode, {} days, {} fills)",
                pass ? "PASSED" : "FAILED", args.mode, days.size(), evidence.canonical_fills);
        LG_DEBUG("Full-run fingerprint {}", evidence.full_run_fingerprint);
        LG_INFO("Summary written to {}", args.output.string());

        return pass ? 0 : 1;
    }
    catch (const std::exception& error) {
        LG_ALERT("Replay stopped by a fatal error: {}", error.what());
        return 2;
    }
}
