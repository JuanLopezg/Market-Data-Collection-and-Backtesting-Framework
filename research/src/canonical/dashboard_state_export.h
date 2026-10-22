#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "market_messages.h"
#include "mock/reconciliation.h"
#include "notional_order_planning.h"

namespace MockVenue {
class MockExchange;
}

// Daily equity points kept in the dashboard state (display only, not economics).
// Large enough for the whole 2020-2025 history (~2100 days) with room to spare.
inline constexpr std::size_t MAX_DASHBOARD_EQUITY_POINTS = 4000;

// Display settings copied from the replay CLI and its strategy configuration.
// The exporter never receives the replay's trading/checkpoint arguments.
struct DashboardExportConfig {
    std::filesystem::path dashboard_state_dir;
    double speed = 1500.0;
    int ui_delay_ms = 0;
    unsigned int liquidity_length = 0U;
    unsigned int liquidity_top_n = 0U;
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

// Shared with the replay summary and checkpoint exports.
std::string jsonEscape(const std::string& input);
std::string reconciliationState(MockVenue::ReconciliationState state);
void atomicWrite(const std::filesystem::path& path, const std::string& data);

// The resume rebuild uses the same daily observations as normal publication.
EquityPoint dashboardDayPoint(
    MockVenue::MockExchange& adapter, const std::string& date, Timestamp ts);

void emitDashboardState(
    MockVenue::MockExchange& adapter,
    const DashboardExportConfig& config,
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
    bool append_equity);
