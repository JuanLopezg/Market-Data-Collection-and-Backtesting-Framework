// Owns durable execution/account state, reconciliation, planning handoff, and exchange-event
// processing.
//
// 1. Restore durable execution state and reconcile it with the exchange snapshot (at startup
//    and, with --periodic-reconciliation, hourly plus 23:50 UTC).
// 2. Turn portfolio decisions into planning requests and consume notional plans.
// 3. Track orders/fills, publish account snapshots, and close each execution cycle only when
//    state is complete.
// 4. Keep the accounting projection the dashboard reads (positions, daily equity, close
//    loop); the daily equity rules are in equity_history.*.
// 5. Execute Manual Control requests from the dashboard (files and limits in manual_requests.*,
//    current prices in binance_prices.*).

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <ctime>
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
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "account.h"
#include "binance_prices.h"
#include "equity_history.h"
#include "manual_requests.h"
#include "position_stops.h"
#include "planning/notional_order_planner.h"
#include "target_decisions.h"
#include "canonical_market_data_reader.h"
#include "message_json.h"
#include "market_messages.h"
#include "decision_batch.h"
#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "execution_engine.h"
#include "execution_messages.h"
#include "planning/planning_state.h"
#include "live_execution_identity.h"
#include "notional_order_planning.h"
#include "adapters/message_exchange.h"
#include "jetstream_bus.h"
#include "postgres_state_store.h"
#include "reconciler.h"
#include "service_logging.h"
#include "trade_recorder.h"
#include "time_handler_factory.h"
#include "time_utils.h"
#include "trading_state_snapshot.h"
#include "message_subjects.h"


// Internal helpers and service implementation.

namespace {

std::atomic<bool> running{true};

void stopHandler(int)
{
    running.store(false);
}

bool envFlag(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
        return false;
    const std::string text(value);
    return !text.empty() && text != "0" && text != "false" && text != "FALSE";
}

Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)
{
    const auto todaySys = std::chrono::floor<std::chrono::days>(timeHandler.getTime());
    const auto previousSys = todaySys - std::chrono::days{1};
    const std::chrono::year_month_day ymd{std::chrono::sys_days{previousSys}};
    const int value = int(ymd.year()) * 10000 +
        static_cast<int>(unsigned(ymd.month())) * 100 +
        static_cast<int>(unsigned(ymd.day()));
    return static_cast<Timestamp>(value);
}

std::optional<Timestamp> configuredReplayBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}


// Command-line configuration.

struct Options {
    std::string nats_url = "nats://127.0.0.1:4222";
    std::string postgres_connection =
        "host=127.0.0.1 port=5432 dbname=algotrading user=algotrading password=algotrading";
    std::string stream = "ALGOTRADING_RUNTIME";
    std::string exchange_control_stream = "ALGOTRADING_EXCHANGE_CONTROL";
    std::string market_data_db = "storage/databases/database.db";
    std::string market_data_source = "binance";
    double initial_cash = 100000.0;
    std::vector<ExecutionStrategyDescriptor> strategies;
    // Re-check the exchange on the wall-clock schedule below, besides the startup check.
    // Off by default so replays and tests keep their exact message sequence.
    bool periodic_reconciliation = false;
    // Manual Control (off when manual_dir is empty): request folder shared with the
    // dashboard, the portfolio config whose risk limits and allocations apply, and the
    // Binance futures API used for the current prices.
    std::string manual_dir;
    std::string portfolio_config;
    std::string price_url = "https://fapi.binance.com";
    // Cash difference accepted by reconciliation. The simulated exchange matches to the
    // cent's millionth; a real venue rounds balances and entry prices (chain LIVE: 0.50).
    double cash_tolerance_usd = 1e-6;
    // LIVE long stop-loss (position_stops.h): a resting venue stop at (1 - fraction) x the
    // latest daily close for every long; 0 = off (PAPER and tests by default).
    double long_stop_fraction = 0.0;
};


// Periodic reconciliation times (UTC, wall clock): every hour from 01:00 to 23:00, plus
// 23:50 so the state is verified shortly before the 00:00 daily cycle. 00:00 itself is
// skipped because orders are in flight then; the 23:50 result already gates that cycle.
std::chrono::system_clock::time_point nextReconciliationAfter(std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;
    const auto day = floor<days>(now);
    std::vector<system_clock::time_point> slots;
    for (int hour = 1; hour <= 23; ++hour)
        slots.push_back(day + hours(hour));
    slots.push_back(day + hours(23) + minutes(50));
    std::sort(slots.begin(), slots.end());
    for (const auto slot : slots)
        if (slot > now)
            return slot;
    return day + days(1) + hours(1);
}


ExecutionStrategyDescriptor parseStrategy(const std::string& value)
{
    const std::size_t separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size())
        throw std::invalid_argument("--strategy must use ID:NAME");

    const unsigned long parsedId = std::stoul(value.substr(0, separator));
    if (parsedId > static_cast<unsigned long>(std::numeric_limits<StrategyID>::max()))
        throw std::invalid_argument("Strategy id is out of range");

    ExecutionStrategyDescriptor descriptor;
    descriptor.strategy_id = static_cast<StrategyID>(parsedId);
    descriptor.name = value.substr(separator + 1);
    return descriptor;
}


Options parseOptions(int argc, char** argv)
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string("Missing value for ") + option);
            return argv[++i];
        };

        if (arg == "--nats-url")
            options.nats_url = requireValue("--nats-url");
        else if (arg == "--postgres")
            options.postgres_connection = requireValue("--postgres");
        else if (arg == "--stream")
            options.stream = requireValue("--stream");
        else if (arg == "--exchange-control-stream")
            options.exchange_control_stream = requireValue("--exchange-control-stream");
        else if (arg == "--market-data-db")
            options.market_data_db = requireValue("--market-data-db");
        else if (arg == "--market-data-source")
            options.market_data_source = requireValue("--market-data-source");
        else if (arg == "--initial-cash")
            options.initial_cash = std::stod(requireValue("--initial-cash"));
        else if (arg == "--strategy")
            options.strategies.push_back(parseStrategy(requireValue("--strategy")));
        else if (arg == "--periodic-reconciliation")
            options.periodic_reconciliation = true;
        else if (arg == "--manual-dir")
            options.manual_dir = requireValue("--manual-dir");
        else if (arg == "--portfolio-config")
            options.portfolio_config = requireValue("--portfolio-config");
        else if (arg == "--price-url")
            options.price_url = requireValue("--price-url");
        else if (arg == "--cash-tolerance-usd")
            options.cash_tolerance_usd = std::stod(requireValue("--cash-tolerance-usd"));
        else if (arg == "--long-stop-fraction")
            options.long_stop_fraction = std::stod(requireValue("--long-stop-fraction"));
        else if (arg == "--help") {
            std::cout
                << "Usage: algotrading_execution_state_service [options]\n"
                << "  --nats-url URL\n"
                << "  --postgres CONNECTION_STRING\n"
                << "  --stream NAME\n"
                << "  --exchange-control-stream NAME\n"
                << "  --market-data-db PATH\n"
                << "  --market-data-source NAME   expected MarketDataUpdated source, default binance\n"
                << "  --initial-cash VALUE\n"
                << "  --strategy ID:NAME   (repeat for each configured strategy)\n"
                << "  --periodic-reconciliation   also reconcile hourly 01:00-23:00 UTC and at 23:50\n"
                << "  --manual-dir PATH           enable Manual Control with this request folder\n"
                << "  --portfolio-config PATH     risk limits/allocations for Manual Control\n"
                << "  --price-url URL             Binance futures API for current prices\n"
                << "  --cash-tolerance-usd VALUE  cash difference accepted by reconciliation, default 1e-6\n"
                << "  --long-stop-fraction VALUE  venue stop-loss this fraction below the last daily close, 0 = off\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("Unknown option: " + arg);
    }

    if (!std::isfinite(options.cash_tolerance_usd) || options.cash_tolerance_usd < 0.0 ||
        options.cash_tolerance_usd > 100.0)
        throw std::invalid_argument("--cash-tolerance-usd must be between 0 and 100");
    if (!std::isfinite(options.long_stop_fraction) || options.long_stop_fraction < 0.0 ||
        options.long_stop_fraction >= 1.0)
        throw std::invalid_argument("--long-stop-fraction must be in [0, 1)");
    if (!options.manual_dir.empty() && options.portfolio_config.empty())
        throw std::invalid_argument("--manual-dir needs --portfolio-config (risk limits)");

    if (options.strategies.empty())
        throw std::invalid_argument("At least one --strategy ID:NAME is required");
    if (options.stream.empty() || options.exchange_control_stream.empty())
        throw std::invalid_argument("Execution-state stream names cannot be empty");
    if (options.initial_cash <= 0.0)
        throw std::invalid_argument("--initial-cash must be positive");
    if (options.market_data_db.empty())
        throw std::invalid_argument("--market-data-db cannot be empty");
    if (options.market_data_source.empty())
        throw std::invalid_argument("--market-data-source cannot be empty");

    std::unordered_set<StrategyID> ids;
    for (const auto& strategy : options.strategies) {
        if (!ids.insert(strategy.strategy_id).second)
            throw std::invalid_argument("Duplicate --strategy id");
    }

    return options;
}


// Service runtime and message-processing loop.

class ExecutionStateServiceRuntime {
private:
    const Options options_;
    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    std::vector<StrategyID> strategy_ids_;
    std::unordered_map<StrategyID, std::string> strategy_names_;

    JetStreamBus bus_;
    CanonicalMarketDataReader market_reader_;
    std::unique_ptr<PostgresStateStore> store_;
    Account account_;
    TradeRecorder trade_recorder_;
    MessageExchange exchange_;
    ExecutionEngine engine_;
    Reconciler reconciler_;
    // Account-level cost basis / realized PnL / fees / funding (see PositionLedger).
    PositionLedger ledger_;

    struct ActiveExecutionCycle {
        Timestamp decision_timestamp = 0;
        Timestamp execution_timestamp = 0;
        std::string correlation_id;
    };

    std::optional<DecisionBatch> pending_decision_;
    std::optional<ActiveExecutionCycle> active_execution_cycle_;
    Timestamp last_decision_timestamp_ = 0;
    bool reconciled_ = false;
    // Position stops already reported as rejected / coins already reported without a close.
    std::unordered_set<OrderID> rejected_stop_alerted_;
    std::set<std::pair<Timestamp, Coin>> stop_close_warned_;
    // Latest venue collateral after haircut (Kraken BTC) from the exchange snapshots; part
    // of the sizing equity (user decision 2026-10-14). Always 0 with the simulated exchange.
    double venue_collateral_usd_ = 0.0;
    std::optional<std::chrono::system_clock::time_point> next_reconciliation_at_;
    // Manual Control (empty when --manual-dir is not given or its setup failed).
    std::unique_ptr<ManualRequestFolder> manual_folder_;
    std::optional<ManualRiskConfig> manual_risk_;
    std::unique_ptr<BinanceLastPrices> price_source_;

    bool chaos_nak_fill_after_persist_once_ =
        envFlag("ALGOTRADING_CHAOS_NAK_FILL_AFTER_PERSIST_ONCE");
    bool chaos_nak_fill_after_persist_done_ = false;

    MessageBus::SubscriptionID exchange_snapshot_subscription_ = 0;
    MessageBus::SubscriptionID exchange_event_subscription_ = 0;
    MessageBus::SubscriptionID decision_subscription_ = 0;
    MessageBus::SubscriptionID notional_plan_subscription_ = 0;
    MessageBus::SubscriptionID market_update_account_subscription_ = 0;

    TradingStateSnapshot snapshot() const
    {
        TradingStateSnapshot result;
        result.last_bar_close_timestamp = last_decision_timestamp_;
        result.last_execution_timestamp = engine_.lastExecutionTimestamp();
        result.next_order_id = engine_.nextOrderId();
        result.account_cash = engine_.account().cash();
        result.account_positions = engine_.account().positions().values();
        result.processed_fill_ids = engine_.orderManager().processedFillIds();

        result.strategies.reserve(options_.strategies.size());
        for (const auto& descriptor : options_.strategies) {
            StrategyStateSnapshot strategy;
            strategy.strategy_id = descriptor.strategy_id;
            strategy.virtual_positions = engine_.strategyPosition(descriptor.strategy_id).values();
            result.strategies.push_back(std::move(strategy));
        }

        if (pending_decision_) {
            result.pending_plans.reserve(pending_decision_->strategies.size());
            for (const StrategyDecisionIntent& intent : pending_decision_->strategies) {
                RebalancePlan plan(intent.decision_timestamp, intent.reference_capital);
                for (const auto& [coin, decision] : intent.decisions)
                    plan.set(coin, decision);
                result.pending_plans.push_back({intent.strategy_id, std::move(plan)});
            }
        }

        result.orders.reserve(engine_.orderManager().orders().size());
        for (const auto& [orderId, order] : engine_.orderManager().orders()) {
            (void)orderId;
            result.orders.push_back(order);
        }
        return result;
    }

    void persist(const std::optional<Fill>& fill = std::nullopt)
    {
        store_->save(snapshot(), fill);
        LG_DEBUG("State saved: cash {}, {} tracked orders, last decision {}, last execution {}, fill {}",
                 FormatUsd(engine_.account().cash()),
                 engine_.orderManager().orders().size(),
                 FormatDayKey(last_decision_timestamp_),
                 FormatDayKey(engine_.lastExecutionTimestamp()),
                 fill ? std::to_string(fill->fill_id) : std::string{"none"});
        if (fill)
            recordFillInLedger(*fill);
    }


    // Accounting projection ----------------------------------------------------------
    // The fills (plus venue fees and funding) are the authority; the position/equity tables are
    // derived from them. A failure here is logged and never stops trading: the next
    // restart rebuilds everything from the fills.

    void recordFillInLedger(const Fill& fill)
    {
        try {
            const std::optional<PositionLot> closed = ledger_.applyFill(fill);
            std::vector<PositionLot> newlyClosed;
            if (closed) {
                newlyClosed.push_back(*closed);
                LG_INFO("Closed {} {} position: net PnL {} (gross {}, fees {}, funding {})",
                        closed->sideName(), closed->coin, FormatUsd(closed->netPnl()),
                        FormatUsd(closed->gross_pnl), FormatUsd(closed->fees), FormatUsd(closed->funding));
            }
            store_->saveLedger(ledger_, newlyClosed);
        }
        catch (const std::exception& error) {
            LG_ERROR("Could not record fill {} in the position ledger (rebuilt on restart): {}", fill.fill_id, error.what());
        }
    }

    // Exact closes of `coins` at close(day) from the market database; nothing (and a
    // warning) when the database cannot provide them, so that day's equity is not recorded.
    std::optional<std::unordered_map<Coin, double>> loadDayCloses(Timestamp day, const std::set<Coin>& coins) const
    {
        try {
            return market_reader_.loadExactClosingPrices(day, coins);
        }
        catch (const std::exception& error) {
            LG_WARN("Equity for {} not recorded: {}", FormatDayKey(day), error.what());
            return std::nullopt;
        }
    }

    // Called when the market update of day T arrives: the account then holds T's open
    // execution but not yet T+1's, so it is the account at close(T). The row is written
    // once; a redelivery of the same update after a restart (when the account may already
    // hold the next open's fills) keeps the first row (see insertEquityDay).
    void recordEquityDay(Timestamp day, double collateralUsd)
    {
        try {
            const Account& account = engine_.account();
            std::set<Coin> coins;
            for (const auto& [coin, quantity] : account.positions().values())
                if (quantity != 0.0)
                    coins.insert(coin);
            const auto closes = loadDayCloses(day, coins);
            if (!closes)
                return;
            const EquityDay row = markEquityDay(day, account.cash(), account.positions().values(), *closes, ledger_,
                                                store_->loadAccountCashMovements(), collateralUsd);
            if (!store_->insertEquityDay(row)) {
                LG_INFO("Equity at the close of {} is already recorded; kept the first row", FormatDayKey(day));
                return;
            }
            LG_INFO("Equity at the close of {}: {} (cash {}, positions {})",
                    FormatDayKey(day), FormatUsd(row.equity), FormatUsd(row.cash), FormatUsd(row.positions_value));
            if (row.collateral_usd > 0.0 || row.capital_flows != 0.0)
                LG_INFO("Beside the equity of {}: venue collateral {} after haircut, net capital flows {}, interest {}",
                        FormatDayKey(day), FormatUsd(row.collateral_usd), FormatUsd(row.capital_flows),
                        FormatUsd(row.interest));
        }
        catch (const std::exception& error) {
            LG_ERROR("Could not record the equity for {}: {}", FormatDayKey(day), error.what());
        }
    }

    // Close loop history for the dashboard: one row per UTC day of the exchange snapshot.
    // Like the other projections, a failure is logged and never stops trading.
    void recordCloseLoop(Timestamp snapshotTimestamp, const ReconciliationReport& report)
    {
        try {
            store_->recordReconciliation(tradingDayOf(snapshotTimestamp), report.clean(), report.issues.size());
        }
        catch (const std::exception& error) {
            LG_ERROR("Could not record the close loop of {}: {}", FormatDayKey(snapshotTimestamp), error.what());
        }
    }

    // Days without an equity row since the first fill (for example right after this table
    // was added, or rows deleted to rebuild them) are rebuilt by replaying the fills from the
    // initial cash. Skipped when that replay does not end at the current account (state
    // restored from another source).
    void backfillEquityHistory(const std::vector<Fill>& fills, const std::vector<FundingPayment>& funding)
    {
        if (fills.empty())
            return;
        const std::vector<VenueCostEvent> movements = store_->loadAccountCashMovements();
        if (!fillsReachAccount(fills, funding, movements, options_.initial_cash, engine_.account())) {
            LG_WARN("Equity history not backfilled: replaying the fills from the initial cash does not reach the current account");
            return;
        }

        const std::vector<EquityDay> rows = equityDaysFromFills(
            fills, funding, movements, options_.initial_cash, newestCompletedUtcDate(time_handler_),
            store_->equityDays(),
            [this](Timestamp day, const std::set<Coin>& coins) { return loadDayCloses(day, coins); },
            [this](Timestamp day) { return store_->dailyCollateral(day).value_or(0.0); });
        std::size_t written = 0;
        for (const EquityDay& row : rows)
            written += store_->insertEquityDay(row) ? 1 : 0;
        if (written > 0)
            LG_INFO("Backfilled {} days of equity history from the fills", written);
    }

    void rebuildAccounting()
    {
        try {
            std::vector<Fill> fills = store_->loadFills();
            // A venue fee reported after its fill (Kraken) joins that fill's commission.
            const std::unordered_map<FillID, double> venueFees = store_->loadFillFees();
            for (Fill& fill : fills)
                if (const auto fee = venueFees.find(fill.fill_id); fee != venueFees.end())
                    fill.commission += fee->second;
            const std::vector<FundingPayment> funding = store_->loadFunding();
            ledger_ = PositionLedger::rebuild(fills, funding);
            store_->replaceLedger(ledger_);
            LG_INFO("Position ledger rebuilt from {} fills: {} open positions, {} closed, realized {}, fees {}, funding {}",
                    fills.size(), ledger_.openPositions().size(), ledger_.closedPositions().size(),
                    FormatUsd(ledger_.realizedGrossPnl()), FormatUsd(ledger_.fees()), FormatUsd(ledger_.funding()));
            backfillEquityHistory(fills, funding);
        }
        catch (const std::exception& error) {
            LG_ERROR("Position ledger rebuild failed; the position/equity tables may be stale: {}", error.what());
        }
    }

    // collateralUsd: the daily sizing snapshot passes the value fixed for its day; the
    // others carry the latest venue value.
    void publishAccountSnapshot(
        Timestamp timestamp,
        const std::string& messageId,
        const std::string& correlationId = {},
        bool resyncToTargets = false,
        std::optional<double> collateralUsd = std::nullopt
    )
    {
        AccountSnapshot value;
        value.metadata.schema_version = 1;
        value.metadata.message_id = messageId;
        value.metadata.correlation_id = correlationId;
        value.metadata.produced_at = timestamp;
        value.timestamp = timestamp;
        value.resync_to_targets = resyncToTargets;
        value.cash = engine_.account().cash();
        value.collateral_usd = collateralUsd.value_or(venue_collateral_usd_);
        value.positions = engine_.account().positions().values();

        for (const auto& strategy : options_.strategies) {
            value.strategy_positions.emplace(
                strategy.strategy_id,
                engine_.strategyPosition(strategy.strategy_id).values()
            );
        }

        bus_.publish(
            MessageSubjects::ACCOUNT_SNAPSHOT,
            MessageJson::encode(value),
            value.metadata.message_id
        );
        LG_INFO("Published account snapshot for {}: cash {}, {} positions, {} strategy position sets",
                FormatDayKey(value.timestamp),
                FormatUsd(value.cash),
                value.positions.size(),
                value.strategy_positions.size());
        LG_DEBUG("Account snapshot message id {}", value.metadata.message_id);
    }

    void maybePublishExecutionCycleComplete()
    {
        if (!active_execution_cycle_)
            return;

        // A terminal Filled OrderUpdate may be delivered by its own durable consumer
        // before the corresponding Fill consumer has applied cash/position state.
        // Therefore "no open orders" alone is not a safe barrier: every Filled order
        // must also have its complete fill quantity recorded locally.
        for (const auto& [orderId, order] : engine_.orderManager().orders()) {
            (void)orderId;
            // A resting position stop stays open across days; it is not part of the cycle.
            if (order.isOpen() && !order.request.isPositionStop())
                return;
            if (order.status == ExecutionOrderStatus::Filled &&
                order.remainingQuantity() > 1e-12)
                return;
        }

        const ActiveExecutionCycle cycle = *active_execution_cycle_;
        ExecutionCycleComplete value;
        value.metadata.schema_version = 1;
        value.metadata.message_id =
            "execution-cycle-complete:" + std::to_string(cycle.decision_timestamp) + ":" +
            std::to_string(cycle.execution_timestamp);
        value.metadata.correlation_id = cycle.correlation_id;
        value.metadata.produced_at = cycle.execution_timestamp;
        value.decision_timestamp = cycle.decision_timestamp;
        value.execution_timestamp = cycle.execution_timestamp;
        value.state_revision = planningState().state_revision;

        bus_.publish(
            MessageSubjects::EXECUTION_CYCLE_COMPLETE,
            MessageJson::encode(value),
            value.metadata.message_id
        );
        LG_INFO("Execution cycle complete: decision {} executed on {}",
                FormatDayKey(value.decision_timestamp), FormatDayKey(value.execution_timestamp));
        LG_DEBUG("Execution cycle state revision {}, message id {}", value.state_revision, value.metadata.message_id);
        active_execution_cycle_.reset();
    }

    void restore(const TradingStateSnapshot& stored)
    {
        if (stored.schema_version != TradingStateSnapshot::CURRENT_SCHEMA_VERSION)
            throw std::invalid_argument("Unsupported execution-state schema version");

        StrategyPositionSnapshot positions;
        for (const StrategyStateSnapshot& strategy : stored.strategies) {
            if (!strategy_names_.contains(strategy.strategy_id))
                throw std::invalid_argument("Stored state contains unknown strategy id");

            VirtualPositionState restored;
            for (const auto& [coin, quantity] : strategy.virtual_positions)
                restored.set(coin, quantity);
            positions.emplace(strategy.strategy_id, std::move(restored));
        }

        engine_.restoreState(
            stored.account_cash,
            stored.account_positions,
            positions,
            stored.orders,
            stored.processed_fill_ids,
            stored.next_order_id,
            stored.last_execution_timestamp
        );

        last_decision_timestamp_ = stored.last_bar_close_timestamp;
        if (stored.last_execution_timestamp > stored.last_bar_close_timestamp) {
            active_execution_cycle_ = ActiveExecutionCycle{
                stored.last_bar_close_timestamp,
                stored.last_execution_timestamp,
                "execution-state-recovery"
            };
        }

        if (!stored.pending_plans.empty()) {
            DecisionBatch restoredBatch;
            restoredBatch.decision_timestamp = stored.pending_plans.front().plan.timestamp();

            for (const PendingPlanSnapshot& pending : stored.pending_plans) {
                if (pending.plan.timestamp() != restoredBatch.decision_timestamp)
                    throw std::invalid_argument(
                        "Execution-state contains multiple pending decision timestamps"
                    );

                StrategyDecisionIntent intent;
                intent.strategy_id = pending.strategy_id;
                intent.decision_timestamp = pending.plan.timestamp();
                intent.reference_capital = pending.plan.referenceCapital();
                intent.decisions = pending.plan.values();
                restoredBatch.strategies.push_back(std::move(intent));
            }

            // A plan that already reached a later execution timestamp was durably applied.
            if (stored.last_execution_timestamp <= restoredBatch.decision_timestamp)
                pending_decision_ = std::move(restoredBatch);
        }

        std::vector<Fill> fills = store_->loadFills();
        std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
            if (a.timestamp != b.timestamp)
                return a.timestamp < b.timestamp;
            return a.fill_id < b.fill_id;
        });

        std::unordered_set<FillID> seen;
        for (const Fill& fill : fills) {
            if (!seen.insert(fill.fill_id).second)
                throw std::invalid_argument("Duplicate fill in PostgreSQL audit trail");
            const auto name = strategy_names_.find(fill.strategy_id);
            if (name == strategy_names_.end())
                throw std::invalid_argument("Persisted fill contains unknown strategy id");
            trade_recorder_.onFill(fill, name->second);
        }
    }

    bool validateDecisionStrategies(const DecisionBatch& batch) const
    {
        std::unordered_set<StrategyID> seen;
        for (const StrategyDecisionIntent& intent : batch.strategies) {
            if (!strategy_names_.contains(intent.strategy_id))
                return false;
            if (!seen.insert(intent.strategy_id).second)
                return false;
            if (intent.decision_timestamp != batch.decision_timestamp)
                return false;
        }
        return true;
    }

    ExecutionPlanningStateSnapshot planningState() const
    {
        return makeExecutionPlanningStateSnapshot(
            strategy_ids_,
            engine_.strategyPositions(),
            engine_.orderManager(),
            engine_.nextOrderId()
        );
    }

    std::set<Coin> notionalPlanningSymbols(const DecisionBatch& decision) const
    {
        std::set<Coin> symbols;

        for (const StrategyDecisionIntent& intent : decision.strategies) {
            for (const auto& [coin, rebalance] : intent.decisions) {
                (void)rebalance;
                symbols.insert(coin);
            }
        }

        for (const auto& [strategyId, positions] : engine_.strategyPositions()) {
            (void)strategyId;
            for (const auto& [coin, quantity] : positions.values()) {
                (void)quantity;
                symbols.insert(coin);
            }
        }

        // Do not require close(T) for unrelated account-level inventory. The notional
        // planner is driven by strategy positions, today's decisions and open strategy
        // orders. Requiring every account asset would make an unrelated/manual holding
        // block the daily planning cycle merely because it is absent from canonical DB.
        for (const auto& [orderId, tracked] : engine_.orderManager().orders()) {
            (void)orderId;
            if (tracked.isOpen())
                symbols.insert(tracked.request.coin);
        }

        return symbols;
    }

    void publishNotionalPlanningRequest(const DecisionBatch& decision)
    {
        const ExecutionPlanningStateSnapshot state = planningState();
        const std::set<Coin> symbols = notionalPlanningSymbols(decision);

        DailyCloseSnapshot closes;
        closes.metadata.schema_version = 1;
        closes.metadata.message_id = "daily-close:" + std::to_string(decision.decision_timestamp);
        closes.metadata.correlation_id = decision.metadata.message_id;
        closes.metadata.produced_at = decision.decision_timestamp;
        closes.date = decision.decision_timestamp;
        closes.closes = market_reader_.loadExactClosingPrices(decision.decision_timestamp, symbols);

        NotionalOrderPlanningRequest request;
        request.metadata.schema_version = 1;
        request.metadata.message_id = LiveExecutionIdentity::notionalPlanningRequest(
            decision.decision_timestamp,
            state.state_revision
        );
        request.metadata.correlation_id = decision.metadata.message_id;
        request.metadata.produced_at = decision.decision_timestamp;
        request.decision_timestamp = decision.decision_timestamp;
        request.decisions = decision;
        request.reference_closes = std::move(closes);
        request.state = state;

        bus_.publish(
            MessageSubjects::NOTIONAL_ORDER_PLANNING_REQUEST,
            MessageJson::encode(request),
            request.metadata.message_id
        );
        bus_.flush();

        LG_INFO("Requested order plan for {} ({} reference prices, {} tracked orders)",
                FormatDayKey(request.decision_timestamp),
                request.reference_closes.closes.size(),
                request.state.orders.size());
        LG_DEBUG("Planning request state revision {}, next order id {}, message id {}",
                 request.state.state_revision, request.state.next_order_id, request.metadata.message_id);
    }

    // The daily plan is requested but not applied yet: a new order id now would change the
    // planning revision the plan was built for, so position stops wait for the plan.
    bool dailyPlanPending() const
    {
        const bool decisionNotExecuted = last_decision_timestamp_ != 0 &&
            engine_.lastExecutionTimestamp() < static_cast<Timestamp>(
                nextDay(static_cast<unsigned int>(last_decision_timestamp_)));
        return pending_decision_.has_value() || decisionNotExecuted;
    }

    // LIVE long stop-loss (position_stops.h): bring the resting venue stop of every long to
    // (1 - fraction) x the latest completed daily close and the current position size.
    // Called after fills, order updates, the applied daily plan and a clean reconciliation.
    void maintainPositionStops(const char* reason)
    {
        if (options_.long_stop_fraction <= 0.0 || !reconciled_ || dailyPlanPending())
            return;

        const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
        const std::optional<Timestamp> closeDay = market_reader_.latestRankingDateAtOrBefore(newestCompleted);
        std::unordered_map<Coin, double> closes;
        if (closeDay) {
            for (const auto& [strategyId, positions] : engine_.strategyPositions()) {
                (void)strategyId;
                for (const auto& [coin, quantity] : positions.values()) {
                    if (quantity <= 0.0 || closes.count(coin))
                        continue;
                    try {
                        const auto found = market_reader_.loadExactClosingPrices(*closeDay, {coin});
                        if (const auto close = found.find(coin); close != found.end())
                            closes[coin] = close->second;
                    }
                    catch (const std::exception&) {
                        // Reported below as a held coin without a close.
                    }
                }
            }
        }

        const Timestamp today = static_cast<Timestamp>(nextDay(static_cast<unsigned int>(newestCompleted)));
        const PositionStopChanges changes = planPositionStops(engine_.strategyPositions(),
            engine_.orderManager().orders(), closes, options_.long_stop_fraction, today, engine_.nextOrderId());

        for (const Coin& coin : changes.missing_closes) {
            if (stop_close_warned_.insert({newestCompleted, coin}).second)
                LG_WARN("No daily close of {} for its stop-loss on {}; its stop is left as it is",
                        coin, FormatDayKey(closeDay.value_or(newestCompleted)));
        }
        for (const OrderID orderId : changes.rejected_stops) {
            if (rejected_stop_alerted_.insert(orderId).second) {
                const TrackedOrder* stop = engine_.orderManager().find(orderId);
                LG_ALERT("Stop-loss order {} for {} was rejected ({}); the position keeps its previous stop, if any",
                         orderId, stop ? stop->request.coin : std::string("?"),
                         stop ? stop->last_message : std::string());
            }
        }
        if (changes.submits.empty() && changes.cancels.empty())
            return;

        engine_.applyPositionStopChanges(changes.submits, changes.cancels, changes.next_order_id, today,
            [this](const std::optional<Fill>& fill) { persist(fill); });
        for (const ExecutionOrder& stop : changes.submits)
            LG_INFO("Stop-loss order {}: SELL {} {} if the mark falls to {} ({:.0f}% below the {} close {}; {})",
                    stop.order_id, FormatQuantity(stop.quantity), stop.coin, FormatQuantity(stop.position_stop_price),
                    options_.long_stop_fraction * 100.0, FormatDayKey(closeDay.value_or(newestCompleted)),
                    FormatQuantity(stop.reference_price), reason);
        for (const OrderID orderId : changes.cancels)
            LG_INFO("Stop-loss order {} cancel requested ({})", orderId, reason);
    }

    void handoffPendingDecisionIfReady()
    {
        if (!reconciled_ || !pending_decision_)
            return;

        const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
        if (pending_decision_->decision_timestamp > newestCompleted)
            return;
        if (!replay_bootstrap_completed_date_.has_value() &&
            pending_decision_->decision_timestamp != newestCompleted)
            return;

        // Persisted pending decision -> durable self-contained NATS planning request ->
        // clear pending marker. A crash after publish but before this second persist only
        // republishes the same deterministic request on recovery.
        publishNotionalPlanningRequest(*pending_decision_);
        pending_decision_.reset();
        persist();
    }

    // Planned USD notional -> executable quantity. Used by the daily plan and by manual
    // rebalances.
    static ExecutionOrder executableOrder(
        const PlannedNotionalOrder& order,
        const ExecutionPlanningStateSnapshot& current,
        Timestamp executionTimestamp)
    {
        if (!std::isfinite(order.reference_close) || order.reference_close <= 0.0 ||
            !std::isfinite(order.notional_usd) || order.notional_usd <= 0.0)
            throw std::invalid_argument("Notional plan order has invalid close(T)/notional");

        // A full FLAT must use the exact filled quantity owned by ExecutionState.
        // Recomputing q as (q * close(T)) / close(T) can leave IEEE-754 dust
        // (for example ~4.5e-13), which then survives as a fake held position.
        // That is economically wrong and can later force PortfolioRisk to request
        // OHLCV for a symbol whose market-data history has legitimately ended.
        double quantity = 0.0;
        if (order.target_notional_usd == 0.0) {
            const auto strategyIt = current.strategy_positions.find(order.strategy_id);
            if (strategyIt == current.strategy_positions.end())
                throw std::invalid_argument("FLAT plan has no current strategy position state");

            const auto positionIt = strategyIt->second.find(order.coin);
            if (positionIt == strategyIt->second.end())
                throw std::invalid_argument("FLAT plan has no current filled position for asset");

            const double currentQuantity = positionIt->second;
            if (!std::isfinite(currentQuantity) || currentQuantity == 0.0)
                throw std::invalid_argument("FLAT plan current filled position is invalid");

            const OrderSide expectedSide =
                currentQuantity > 0.0 ? OrderSide::Sell : OrderSide::Buy;
            if (order.side != expectedSide)
                throw std::invalid_argument("FLAT plan side does not flatten current filled position");

            quantity = std::abs(currentQuantity);
        } else {
            // Non-flat sizing remains exactly the established T17 rule: convert
            // economic notional with the exact canonical close(T). Fill price is
            // independent exchange truth supplied by the historical T+1 OPEN.
            quantity = order.notional_usd / order.reference_close;
        }

        if (!std::isfinite(quantity) || quantity <= 0.0)
            throw std::invalid_argument("Notional plan converts to invalid quantity");

        ExecutionOrder executable(
            order.order_id,
            order.strategy_id,
            order.created_at,
            executionTimestamp,
            order.coin,
            order.side,
            quantity
        );
        // The sizing price. A real venue checks the order's USD value against it.
        executable.reference_price = order.reference_close;
        return executable;
    }

    DurableMessageDisposition onNotionalOrderPlan(const BusMessage& message)
    {
        try {
            if (!reconciled_)
                return DurableMessageDisposition::Retry;

            const NotionalOrderPlanBatch plan =
                MessageJson::decodeNotionalOrderPlanBatch(message.payload);

            const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
            if (plan.decision_timestamp > newestCompleted) {
                LG_DEBUG("Order plan for {} waits for business time (newest completed day {}); will retry",
                         FormatDayKey(plan.decision_timestamp), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Retry;
            }

            const Timestamp executionTimestamp = static_cast<Timestamp>(
                nextDay(static_cast<unsigned int>(plan.decision_timestamp)));

            // Durable idempotency: applyOrderPlan persists last_execution_timestamp before
            // commands leave this process.  A redelivery after that boundary must ACK,
            // never allocate/submit the same economics a second time.
            if (engine_.lastExecutionTimestamp() >= executionTimestamp) {
                LG_INFO("Order plan for {} is already applied or stale (execution {}, last execution {}); ignoring it",
                        FormatDayKey(plan.decision_timestamp),
                        FormatDayKey(executionTimestamp),
                        FormatDayKey(engine_.lastExecutionTimestamp()));
                return DurableMessageDisposition::Ack;
            }

            const ExecutionPlanningStateSnapshot current = planningState();
            if (current.state_revision != plan.state_revision) {
                LG_WARN("Order plan for {} was built for state revision {}, but the current revision is {}; will retry",
                        FormatDayKey(plan.decision_timestamp), plan.state_revision, current.state_revision);
                return DurableMessageDisposition::Retry;
            }

            if (plan.next_order_id < current.next_order_id)
                throw std::invalid_argument("Notional plan next_order_id regresses execution state");

            OrderPlanBatch executable;
            executable.metadata = plan.metadata;
            executable.decision_timestamp = plan.decision_timestamp;
            executable.execution_timestamp = executionTimestamp;
            executable.state_revision = plan.state_revision;
            executable.decisions = plan.decisions;
            executable.next_order_id = plan.next_order_id;
            executable.cancel_order_ids = plan.cancel_order_ids;
            executable.global_target_exposure = plan.global_target_notional_usd;

            executable.submit_orders.reserve(plan.submit_orders.size());
            for (const PlannedNotionalOrder& order : plan.submit_orders) {
                if (order.decision_timestamp != plan.decision_timestamp ||
                    order.created_at != plan.decision_timestamp ||
                    order.state_revision != plan.state_revision)
                    throw std::invalid_argument("Notional plan order identity mismatch");
                executable.submit_orders.push_back(executableOrder(order, current, executionTimestamp));
            }

            engine_.applyOrderPlan(
                executable,
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );

            active_execution_cycle_ = ActiveExecutionCycle{
                plan.decision_timestamp,
                executionTimestamp,
                plan.metadata.message_id
            };

            LG_INFO("Applied order plan for {} on {}: {} new orders, {} cancels",
                    FormatDayKey(plan.decision_timestamp),
                    FormatDayKey(executionTimestamp),
                    executable.submit_orders.size(),
                    plan.cancel_order_ids.size());
            maintainPositionStops("daily close"); // moves the stop of every held long to the new close
            LG_DEBUG("Applied plan state revision {}, next order id {}, message id {}",
                     plan.state_revision, plan.next_order_id, plan.metadata.message_id);

            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid order plan: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Applying the order plan failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    void requestExchangeSnapshot(const std::string& reason = "startup")
    {
        ExchangeSnapshotRequest request;
        request.metadata.schema_version = 1;
        request.metadata.correlation_id = "execution-state-" + reason;
        request.metadata.produced_at = engine_.lastExecutionTimestamp();

        const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();
        request.metadata.message_id =
            "exchange-snapshot-request:" + std::to_string(nonce);

        bus_.publish(
            MessageSubjects::EXCHANGE_SNAPSHOT_REQUEST,
            MessageJson::encode(request),
            request.metadata.message_id
        );
        LG_INFO("Requested exchange snapshot ({}, last execution {})", reason, FormatDayKey(engine_.lastExecutionTimestamp()));
        LG_DEBUG("Exchange snapshot request message id {}", request.metadata.message_id);
    }

    // Scheduled re-check of the exchange (see nextReconciliationAfter). Skipped while an
    // execution cycle runs or any order is open: in-flight orders and fills would show as
    // differences that are not drift. A mismatch pauses trading exactly like the startup
    // check; the next clean scheduled check resumes it.
    void maybeRequestPeriodicReconciliation()
    {
        if (!options_.periodic_reconciliation)
            return;
        const auto now = std::chrono::system_clock::now();
        if (!next_reconciliation_at_) {
            next_reconciliation_at_ = nextReconciliationAfter(now);
            return;
        }
        if (now < *next_reconciliation_at_)
            return;
        next_reconciliation_at_ = nextReconciliationAfter(now);

        const bool ordersOpen = std::any_of(
            engine_.orderManager().orders().begin(), engine_.orderManager().orders().end(),
            [](const auto& entry) { return entry.second.isOpen() && !entry.second.request.isPositionStop(); });
        if (active_execution_cycle_ || ordersOpen) {
            LG_INFO("Skipped the scheduled reconciliation: an execution cycle or open orders are in flight");
            return;
        }
        requestExchangeSnapshot("scheduled");
    }

    // Manual Control ---------------------------------------------------------------------
    // The dashboard drops a target portfolio (asset weights of equity) in the request
    // folder. It executes at once at the current Binance prices, through the normal
    // planner and exchange path, or is rejected with its reason: nothing waits in a queue,
    // so an old request can never execute later. The next daily account snapshot then
    // asks risk to return to the strategy targets.

    struct ManualRejected : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    static constexpr auto MANUAL_REQUEST_MAX_AGE = std::chrono::minutes(2);
    static constexpr auto MANUAL_PRICE_MAX_AGE = std::chrono::minutes(5);

    Timestamp todayKey() const
    {
        return static_cast<Timestamp>(toYYYYMMDD(getCurrentUtcDate(time_handler_.getTime())));
    }

    void setupManualControl()
    {
        if (options_.manual_dir.empty())
            return;
        try {
            if (replay_bootstrap_completed_date_.has_value())
                throw std::invalid_argument("not available in a historical replay");
            manual_risk_ = loadManualRiskConfig(options_.portfolio_config, strategy_ids_);
            manual_folder_ = std::make_unique<ManualRequestFolder>(options_.manual_dir);
            price_source_ = std::make_unique<BinanceLastPrices>(options_.price_url);
            LG_INFO("Manual Control enabled: requests in {}, limits {:.0f}% in total and {:.0f}% per asset",
                    options_.manual_dir, manual_risk_->max_gross_leverage * 100.0,
                    manual_risk_->max_asset_weight * 100.0);
        }
        catch (const std::exception& error) {
            manual_folder_.reset();
            manual_risk_.reset();
            price_source_.reset();
            LG_ALERT("Manual Control is disabled: {}", error.what());
        }
    }

    // Current Binance price of every asset in `assets`; rejects when one is missing or old.
    ExecutionReferencePrices currentPrices(const std::set<Coin>& assets) const
    {
        std::unordered_map<Coin, LastPrice> prices;
        try {
            prices = price_source_->fetchAll();
        }
        catch (const std::exception& error) {
            throw ManualRejected(std::string("Could not read current Binance prices: ") + error.what());
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto maxAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(MANUAL_PRICE_MAX_AGE).count();

        ExecutionReferencePrices result;
        for (const Coin& asset : assets) {
            const auto it = prices.find(asset);
            if (it == prices.end())
                throw ManualRejected("No current Binance price for " + asset);
            if (it->second.time_ms != 0 && nowMs - it->second.time_ms > maxAgeMs)
                throw ManualRejected("The last Binance trade of " + asset + " is more than 5 minutes old");
            result.set(asset, it->second.price);
        }
        return result;
    }

    void checkManualRequestAllowed(const ManualRequest& request) const
    {
        const auto now = std::chrono::system_clock::now();
        const auto created = parseUtcSeconds(request.created_at);
        if (!created || now - *created > MANUAL_REQUEST_MAX_AGE || *created - now > std::chrono::minutes(1))
            throw ManualRejected("The request is more than 2 minutes old; send it again");
        if (!reconciled_)
            throw ManualRejected("Reconciliation with the exchange is not clean");
        if (inDailyCycleWindow(now))
            throw ManualRejected("No manual orders between 23:45 and 00:15 UTC (daily cycle)");
        const bool decisionNotExecuted = last_decision_timestamp_ != 0 &&
            engine_.lastExecutionTimestamp() < static_cast<Timestamp>(
                nextDay(static_cast<unsigned int>(last_decision_timestamp_)));
        if (active_execution_cycle_ || pending_decision_ || decisionNotExecuted)
            throw ManualRejected("The daily cycle is still running; try again when its orders are filled");
        for (const auto& [orderId, order] : engine_.orderManager().orders()) {
            (void)orderId;
            if (order.isOpen() && !order.request.isPositionStop()) // stop-losses keep resting
                throw ManualRejected("Orders are still open at the exchange");
        }

        double gross = 0.0;
        for (const auto& [asset, weight] : request.weights) {
            if (weight > manual_risk_->max_asset_weight + 1e-9)
                throw ManualRejected(asset + " is above the risk limit per asset");
            gross += weight;
        }
        if (gross > manual_risk_->max_gross_leverage + 1e-9)
            throw ManualRejected("The weights add up to more than the risk limit in total");
    }

    // Plans and sends one request. Returns its outcome; throws ManualRejected.
    ManualOutcome executeManualRequest(const ManualRequest& request)
    {
        checkManualRequestAllowed(request);
        const Timestamp today = todayKey();

        // Every asset the plan touches: targets, account holdings and strategy holdings.
        std::set<Coin> assets;
        for (const auto& [asset, weight] : request.weights) {
            (void)weight;
            assets.insert(asset);
        }
        for (const auto& [asset, quantity] : engine_.account().positions().values())
            if (quantity != 0.0)
                assets.insert(asset);
        for (const auto& [strategyId, positions] : engine_.strategyPositions()) {
            (void)strategyId;
            for (const auto& [asset, quantity] : positions.values())
                if (quantity != 0.0)
                    assets.insert(asset);
        }
        const ExecutionReferencePrices prices = currentPrices(assets);

        // Same capital as the daily sizing: cash, venue collateral after haircut, positions.
        double equity = engine_.account().cash() + venue_collateral_usd_;
        for (const auto& [asset, quantity] : engine_.account().positions().values())
            if (quantity != 0.0)
                equity += quantity * prices.get(asset);
        if (!std::isfinite(equity) || equity <= 0.0)
            throw ManualRejected("Account equity is not positive");

        DecisionBatch decisions;
        decisions.decision_timestamp = today;
        for (const auto& strategy : options_.strategies) {
            StrategyDecisionIntent intent;
            intent.strategy_id = strategy.strategy_id;
            intent.decision_timestamp = today;
            intent.reference_capital = equity * manual_risk_->allocation.at(strategy.strategy_id);
            setTargetDecisions(intent, request.weights, &engine_.strategyPosition(strategy.strategy_id), prices);
            decisions.strategies.push_back(std::move(intent));
        }

        const ExecutionPlanningStateSnapshot state = planningState();
        const NotionalOrderPlannerResult plan = NotionalOrderPlanner().createPlan(
            strategy_ids_, engine_.strategyPositions(), engine_.orderManager(),
            today, state.state_revision, prices, decisions, engine_.nextOrderId());
        if (!plan.cancel_order_ids.empty())
            throw ManualRejected("Orders are still open at the exchange");

        ManualOutcome outcome;
        outcome.executed = true;
        outcome.execution_day = today;
        if (plan.submit_orders.empty()) {
            outcome.detail = "Already at these weights; no order was needed";
            return outcome;
        }

        std::vector<ExecutionOrder> orders;
        for (const PlannedNotionalOrder& planned : plan.submit_orders) {
            ExecutionOrder order = executableOrder(planned, state, today);
            order.fill_at_reference_price = true;
            orders.push_back(std::move(order));
        }
        outcome.first_order = orders.front().order_id;
        outcome.last_order = orders.back().order_id;
        outcome.detail = std::to_string(orders.size()) + " orders sent at the current Binance prices (equity " +
            FormatUsd(equity) + ")";

        // Durable before anything leaves: re-sync marker, then the processed record (so the
        // request can never run twice), then the orders (staged, persisted, submitted).
        manual_folder_->markResyncPending();
        manual_folder_->finish(request.file, request.raw_lines, outcome);
        for (const ExecutionOrder& order : orders)
            LG_INFO("Manual order {}: {} {} {} at about {} for strategy {}",
                    order.order_id, order.side == OrderSide::Buy ? "BUY" : "SELL",
                    FormatQuantity(order.quantity), order.coin, FormatQuantity(order.reference_price),
                    order.strategy_id);
        engine_.applyManualOrders(orders, plan.next_order_id, today,
                                  [this](const std::optional<Fill>& fill) { persist(fill); });
        return outcome;
    }

    void processManualRequest()
    {
        if (!manual_folder_)
            return;

        std::optional<std::filesystem::path> file;
        try {
            file = manual_folder_->nextRequest();
        }
        catch (const std::exception& error) {
            LG_ERROR("Cannot read the manual request folder: {}", error.what());
            return;
        }
        if (!file)
            return;
        if (manual_folder_->alreadyProcessed(*file)) {
            LG_WARN("Manual request {} was already processed; removing the copy", file->filename().string());
            manual_folder_->remove(*file);
            return;
        }

        ManualRequest request;
        try {
            request = manual_folder_->read(*file);
            LG_INFO("Manual request {} from {}: {} target assets", request.request_id, request.actor,
                    request.weights.size());
            const ManualOutcome outcome = executeManualRequest(request);
            if (outcome.last_order == 0)
                manual_folder_->finish(*file, request.raw_lines, outcome);
            LG_INFO("Manual request {} executed: {}", request.request_id, outcome.detail);
        }
        catch (const ManualRejected& rejected) {
            LG_WARN("Manual request {} rejected: {}", file->filename().string(), rejected.what());
            finishRejected(*file, request.raw_lines, rejected.what());
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Manual request {} rejected: {}", file->filename().string(), error.what());
            finishRejected(*file, request.raw_lines, std::string("Invalid request: ") + error.what());
        }
        catch (const std::exception& error) {
            // After the processed record the request never runs again; orders already
            // staged are recovered like any other unsent order.
            LG_ALERT("Manual request {} failed: {}", file->filename().string(), error.what());
            if (std::filesystem::exists(*file))
                finishRejected(*file, request.raw_lines, std::string("Failed: ") + error.what());
        }
    }

    void finishRejected(const std::filesystem::path& file, const std::vector<std::string>& lines,
                        const std::string& reason)
    {
        try {
            ManualOutcome outcome;
            outcome.detail = reason;
            manual_folder_->finish(file, lines, outcome);
        }
        catch (const std::exception& error) {
            LG_ERROR("Cannot record the manual request outcome: {}", error.what());
            manual_folder_->remove(file);
        }
    }

    void recoverOutboundIntents()
    {
        // Created orders were durably staged before submit publication. Re-publishing the
        // same OrderID is intentionally idempotent at the transport/gateway boundary.
        std::vector<OrderID> created;
        std::vector<OrderID> cancels;

        for (const auto& [orderId, tracked] : engine_.orderManager().orders()) {
            if (tracked.status == ExecutionOrderStatus::Created)
                created.push_back(orderId);
            if (tracked.cancel_requested && tracked.isOpen())
                cancels.push_back(orderId);
        }
        std::sort(created.begin(), created.end());
        std::sort(cancels.begin(), cancels.end());

        for (const OrderID orderId : created) {
            const TrackedOrder* tracked = engine_.orderManager().find(orderId);
            if (!tracked)
                throw std::logic_error("Created recovery order disappeared");
            exchange_.submitOrder(tracked->request);
            engine_.orderManager().markSubmitted(orderId, tracked->request.created_at);
            persist();
        }

        for (const OrderID orderId : cancels)
            exchange_.cancelOrder(orderId);
    }

    DurableMessageDisposition onExchangeSnapshot(const BusMessage& message)
    {
        try {
            const ExchangeSnapshotEvent value =
                MessageJson::decodeExchangeSnapshotEvent(message.payload);
            if (value.snapshot.timestamp == 0)
                return DurableMessageDisposition::Terminate;

            if (value.snapshot.collateral_usd != venue_collateral_usd_)
                LG_INFO("Venue collateral after haircut: {} (was {})",
                        FormatUsd(value.snapshot.collateral_usd), FormatUsd(venue_collateral_usd_));
            venue_collateral_usd_ = value.snapshot.collateral_usd;

            const ReconciliationReport report = reconciler_.compare(snapshot(), value.snapshot);
            recordCloseLoop(value.snapshot.timestamp, report);
            if (!report.clean()) {
                reconciled_ = false;
                LG_ALERT("Reconciliation with the exchange snapshot of {} found {} issues; trading is paused",
                         FormatDayKey(value.snapshot.timestamp), report.issues.size());
                for (const ReconciliationIssue& issue : report.issues) {
                    LG_WARN("Reconciliation issue on {} (order {}): {} - local {}, exchange {}",
                            issue.coin.empty() ? std::string{"account"} : issue.coin,
                            issue.order_id,
                            issue.message,
                            FormatQuantity(issue.local_value),
                            FormatQuantity(issue.exchange_value));
                }
                return DurableMessageDisposition::Ack;
            }

            if (reconciled_ && options_.periodic_reconciliation) {
                // A scheduled check confirming already reconciled state: nothing to resume
                // or republish. (Without the schedule the original path below is kept.)
                LG_INFO("Reconciliation still clean: cash {}, {} positions, {} open orders",
                        FormatUsd(value.snapshot.cash),
                        value.snapshot.positions.size(),
                        value.snapshot.open_orders.size());
                return DurableMessageDisposition::Ack;
            }

            reconciled_ = true;
            LG_INFO("Reconciliation clean for {}: cash {}, {} positions, {} open orders",
                    FormatDayKey(value.snapshot.timestamp),
                    FormatUsd(value.snapshot.cash),
                    value.snapshot.positions.size(),
                    value.snapshot.open_orders.size());
            recoverOutboundIntents();
            publishAccountSnapshot(
                value.snapshot.timestamp,
                "account-snapshot:reconciled:" + std::to_string(value.snapshot.timestamp),
                value.metadata.message_id
            );
            handoffPendingDecisionIfReady();
            maintainPositionStops("reconciliation"); // e.g. the first start with stops enabled
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("Exchange snapshot handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onMarketDataUpdatedForAccountSnapshot(const BusMessage& message)
    {
        try {
            if (!reconciled_)
                return DurableMessageDisposition::Retry;

            const MarketDataUpdated update = MessageJson::decodeMarketDataUpdated(message.payload);
            if (update.completed_through == 0 || update.source != options_.market_data_source || update.timeframe != "1d")
                return DurableMessageDisposition::Terminate;

            const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
            if (update.completed_through > newestCompleted)
                return DurableMessageDisposition::Terminate;
            if (replay_bootstrap_completed_date_.has_value() &&
                update.completed_through < *replay_bootstrap_completed_date_) {
                LG_INFO("Skipped market update for {}: it is before the bootstrap day {}",
                        FormatDayKey(update.completed_through), FormatDayKey(*replay_bootstrap_completed_date_));
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                update.completed_through < newestCompleted) {
                LG_INFO("Skipped stale market update for {} (newest completed day {})",
                        FormatDayKey(update.completed_through), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Ack;
            }

            // Historical replay must make the close-T account snapshot causal with
            // execution at T.  Orders decided on T-1 execute at open(T), so publishing
            // AccountSnapshot(T) before that execution cycle has been applied and fully
            // consumed makes PortfolioRisk depend on CPU/NATS latency.  The immutable
            // bootstrap day is the only exception: it intentionally exposes the initial
            // account before any replay execution cycle exists.
            if (replay_bootstrap_completed_date_.has_value() &&
                update.completed_through > *replay_bootstrap_completed_date_) {
                if (engine_.lastExecutionTimestamp() < update.completed_through) {
                    LG_DEBUG("Account snapshot for {} waits for the order plan (last execution {}); will retry",
                             FormatDayKey(update.completed_through), FormatDayKey(engine_.lastExecutionTimestamp()));
                    return DurableMessageDisposition::Retry;
                }

                if (active_execution_cycle_.has_value() &&
                    active_execution_cycle_->execution_timestamp <= update.completed_through) {
                    LG_DEBUG("Account snapshot for {} waits for the execution cycle of {} to finish; will retry",
                             FormatDayKey(update.completed_through), FormatDayKey(active_execution_cycle_->execution_timestamp));
                    return DurableMessageDisposition::Retry;
                }
            }

            // Venue collateral for day T's sizing, fixed at its first use so a redelivered
            // update republishes the identical snapshot (risk rejects a different one).
            const double collateral = venue_collateral_usd_ > 0.0
                ? store_->fixDailyCollateral(update.completed_through, venue_collateral_usd_)
                : store_->dailyCollateral(update.completed_through).value_or(0.0);

            // The account at close T (after T's open execution) marked at close(T).
            recordEquityDay(update.completed_through, collateral);

            // After a manual rebalance this snapshot asks risk to return to the strategy
            // targets (the marker keeps the answer stable if the update is redelivered).
            const bool resync = manual_folder_ && manual_folder_->resyncForDay(update.completed_through);
            if (resync)
                LG_INFO("Account snapshot for {} asks risk to return to the strategy targets after a manual rebalance",
                        FormatDayKey(update.completed_through));

            // LIVE market-data no longer publishes MarketSliceSnapshot. This small bridge
            // exposes the already durable ExecutionState account at close T so
            // PortfolioRisk can join StrategyIntent(T) + AccountSnapshot(T).
            publishAccountSnapshot(
                update.completed_through,
                "account-snapshot:market-data:" + std::to_string(update.completed_through),
                update.metadata.message_id,
                resync,
                collateral
            );

            LG_INFO("Published account snapshot after the {} {} market update for {}",
                    update.source, update.timeframe, FormatDayKey(update.completed_through));
            LG_DEBUG("Account snapshot correlation id {}", update.metadata.message_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("Account snapshot after the market update failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onDecision(const BusMessage& message)
    {
        try {
            const DecisionBatch batch = MessageJson::decodeDecisionBatch(message.payload);
            if (batch.decision_timestamp == 0 || !validateDecisionStrategies(batch))
                return DurableMessageDisposition::Terminate;

            const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
            if (batch.decision_timestamp > newestCompleted) {
                LG_WARN("Rejected decision for {}: that day is not complete yet (newest completed day {})",
                        FormatDayKey(batch.decision_timestamp), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Terminate;
            }
            if (replay_bootstrap_completed_date_.has_value() &&
                batch.decision_timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO("Skipped decision for {}: it is before the bootstrap day {}",
                        FormatDayKey(batch.decision_timestamp), FormatDayKey(*replay_bootstrap_completed_date_));
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                batch.decision_timestamp < newestCompleted) {
                LG_INFO("Skipped stale decision for {} (newest completed day {})",
                        FormatDayKey(batch.decision_timestamp), FormatDayKey(newestCompleted));
                return DurableMessageDisposition::Ack;
            }

            if (batch.decision_timestamp < last_decision_timestamp_)
                return DurableMessageDisposition::Ack;

            if (batch.decision_timestamp == last_decision_timestamp_) {
                // A crash can leave the decision durably pending after the NATS message
                // was already ACKed. Re-run only the handoff; deterministic MessageID
                // keeps transport idempotent.
                if (pending_decision_ &&
                    pending_decision_->decision_timestamp == batch.decision_timestamp)
                    handoffPendingDecisionIfReady();
                return DurableMessageDisposition::Ack;
            }

            if (pending_decision_)
                return DurableMessageDisposition::Retry;

            last_decision_timestamp_ = batch.decision_timestamp;
            pending_decision_ = batch;
            persist();

            LG_INFO("Saved decision for {} ({} strategies, reconciled: {})",
                    FormatDayKey(batch.decision_timestamp), batch.strategies.size(), YesNo(reconciled_));
            LG_DEBUG("Decision message id {}", batch.metadata.message_id);

            handoffPendingDecisionIfReady();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::logic_error& error) {
            LG_WARN("Decision waits for market reference prices; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
        catch (const std::exception& error) {
            LG_ERROR("Decision handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onOrderUpdate(const BusMessage& message)
    {
        try {
            const OrderUpdateEvent value = MessageJson::decodeOrderUpdateEvent(message.payload);
            const bool applied = engine_.processExchangeEvent(
                ExchangeEvent{value.update},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            if (!applied) {
                // A redelivered/replayed update for a status this order has already passed.
                LG_DEBUG("Ignored stale update: order {} already past {} ({})",
                         value.update.order_id, executionOrderStatusName(value.update.status),
                         FormatDayKey(value.update.timestamp));
                return DurableMessageDisposition::Ack;
            }
            LG_INFO("Order {} is now {} ({}){}",
                    value.update.order_id,
                    executionOrderStatusName(value.update.status),
                    FormatDayKey(value.update.timestamp),
                    value.update.message.empty() ? std::string{} : " - " + value.update.message);
            LG_DEBUG("Order {} exchange order id {}", value.update.order_id, value.update.exchange_order_id);
            maintainPositionStops("order update");
            maybePublishExecutionCycleComplete();
            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("Order update handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onFill(const BusMessage& message)
    {
        try {
            const FillEvent value = MessageJson::decodeFillEvent(message.payload);
            LG_INFO("Fill received: {} {} {} at {} for {} on {} (order {}, commission {})",
                    value.fill.side == OrderSide::Buy ? "BUY" : "SELL",
                    FormatQuantity(value.fill.quantity),
                    value.fill.coin,
                    FormatQuantity(value.fill.price),
                    value.fill.strategy_id,
                    FormatDayKey(value.fill.timestamp),
                    value.fill.order_id,
                    FormatQuantity(value.fill.commission));
            LG_DEBUG("Fill id {} for order {}", value.fill.fill_id, value.fill.order_id);
            const std::size_t processedBefore =
                engine_.orderManager().processedFillIds().size();
            engine_.processExchangeEvent(
                ExchangeEvent{value.fill},
                [this](const std::optional<Fill>& fill) { persist(fill); }
            );
            const std::size_t processedAfter =
                engine_.orderManager().processedFillIds().size();

            publishAccountSnapshot(
                value.fill.timestamp,
                "account-snapshot:fill:" + std::to_string(value.fill.fill_id),
                value.metadata.message_id
            );

            if (processedAfter == processedBefore) {
                LG_WARN("Fill {} for order {} was already applied; ignoring the duplicate",
                        value.fill.fill_id, value.fill.order_id);
                LG_DEBUG("Duplicate fill transport message id {}", value.metadata.message_id);
            }
            else {
                LG_INFO("Applied fill {}: cash is now {}, {} open positions",
                        value.fill.fill_id,
                        FormatUsd(engine_.account().cash()),
                        engine_.account().positions().values().size());
                LG_DEBUG("Processed fill count is now {}", processedAfter);
                maintainPositionStops("fill");
            }

            maybePublishExecutionCycleComplete();

            if (chaos_nak_fill_after_persist_once_ &&
                !chaos_nak_fill_after_persist_done_) {
                chaos_nak_fill_after_persist_done_ = true;
                LG_WARN("Fault test: refusing fill {} (order {}) after saving it, to check redelivery",
                        value.fill.fill_id, value.fill.order_id);
                return DurableMessageDisposition::Retry;
            }

            return DurableMessageDisposition::Ack;
        }
        catch (const std::exception& error) {
            LG_ERROR("Fill handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    // A venue fee reported after its fill, a funding payment, venue interest or a USD
    // capital flow (deposit, withdrawal, conversion) - real venues only; the PAPER simulation
    // charges its commission inside the fill. Applied to cash once: the cost row and the
    // snapshot with the new cash commit together. The position ledger is then rebuilt so
    // a fee joins its fill and funding its position; interest and flows stay account-level.
    DurableMessageDisposition onVenueCost(const BusMessage& message)
    {
        try {
            const VenueCostEvent cost = MessageJson::decodeVenueCostEvent(message.payload);
            if (store_->knowsVenueCost(cost.cost_id)) {
                LG_DEBUG("Venue cost {} was already applied; ignoring the duplicate", cost.cost_id);
                return DurableMessageDisposition::Ack;
            }

            engine_.account().applyCashMovement(cost.amount_usd);
            try {
                store_->saveVenueCost(snapshot(), cost);
            }
            catch (...) {
                engine_.account().applyCashMovement(-cost.amount_usd);
                throw;
            }
            LG_INFO("Applied venue {} {} on {}{}: cash is now {}",
                    venueCostKindLabel(cost.kind), FormatUsd(cost.amount_usd),
                    cost.coin.empty() ? std::string{"the account"} : cost.coin,
                    cost.fill_id != 0 ? " (fill " + std::to_string(cost.fill_id) + ")" : std::string{},
                    FormatUsd(engine_.account().cash()));
            LG_DEBUG("Venue cost id {}", cost.cost_id);

            rebuildAccounting();
            publishAccountSnapshot(cost.timestamp, "account-snapshot:cost:" + cost.cost_id, cost.metadata.message_id);
            return DurableMessageDisposition::Ack;
        }
        catch (const std::invalid_argument& error) {
            LG_WARN("Rejected invalid venue cost: {}", error.what());
            return DurableMessageDisposition::Terminate;
        }
        catch (const std::exception& error) {
            LG_ERROR("Venue cost handling failed; will retry: {}", error.what());
            return DurableMessageDisposition::Retry;
        }
    }

    DurableMessageDisposition onExchangeEvent(const BusMessage& message)
    {
        if (message.subject == MessageSubjects::ORDER_UPDATE)
            return onOrderUpdate(message);
        if (message.subject == MessageSubjects::FILL)
            return onFill(message);
        if (message.subject == MessageSubjects::VENUE_COST)
            return onVenueCost(message);
        return DurableMessageDisposition::Terminate;
    }

    DurableConsumerOptions consumer(const std::string& durable, const std::string& subject) const
    {
        DurableConsumerOptions result;
        result.stream = options_.stream;
        result.durable_name = durable;
        result.subject = subject;
        result.ack_wait_ms = 30000;
        result.max_deliver = 20;
        result.max_ack_pending = 256;
        return result;
    }

public:
    explicit ExecutionStateServiceRuntime(Options options)
        : options_(std::move(options)),
          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
          market_reader_(options_.market_data_db),
          account_(options_.initial_cash),
          exchange_(bus_),
          engine_(options_.strategies, account_, trade_recorder_, exchange_),
          reconciler_(options_.cash_tolerance_usd)
    {
        for (const auto& strategy : options_.strategies) {
            strategy_ids_.push_back(strategy.strategy_id);
            strategy_names_.emplace(strategy.strategy_id, strategy.name);
        }

        bus_.ensureStream(options_.stream, MessageSubjects::tradingRuntimeSubjects());
        bus_.ensureStream(
            options_.exchange_control_stream,
            MessageSubjects::exchangeGatewayControlSubjects()
        );

        store_ = std::make_unique<PostgresStateStore>(options_.postgres_connection);

        if (const auto stored = store_->load()) {
            LG_INFO("Found saved state; restoring it");
            restore(*stored);
            LG_INFO("Restored state: cash {}, {} positions, {} tracked orders, last decision {}, last execution {}",
                    FormatUsd(engine_.account().cash()),
                    engine_.account().positions().values().size(),
                    engine_.orderManager().orders().size(),
                    FormatDayKey(last_decision_timestamp_),
                    FormatDayKey(engine_.lastExecutionTimestamp()));
        }
        else {
            LG_INFO("No saved state found; starting with {} cash", FormatUsd(options_.initial_cash));
            persist();
        }
        rebuildAccounting();
        setupManualControl();

        exchange_snapshot_subscription_ = bus_.subscribe(
            consumer("execution-state-exchange-snapshot", MessageSubjects::EXCHANGE_SNAPSHOT),
            [this](const BusMessage& message) { return onExchangeSnapshot(message); }
        );
        // Exchange truth remains continuously processed even though economic decisions
        // are daily. Fills/reconciliation are safety state, not a once-a-day signal.
        exchange_event_subscription_ = bus_.subscribe(
            consumer("execution-state-exchange-events", MessageSubjects::EXECUTION_EVENT_FILTER),
            [this](const BusMessage& message) { return onExchangeEvent(message); }
        );
        decision_subscription_ = bus_.subscribe(
            consumer("execution-state-live-decisions", MessageSubjects::DECISION_BATCH),
            [this](const BusMessage& message) { return onDecision(message); }
        );
        notional_plan_subscription_ = bus_.subscribe(
            consumer("execution-state-notional-plans", MessageSubjects::NOTIONAL_ORDER_PLAN),
            [this](const BusMessage& message) { return onNotionalOrderPlan(message); }
        );
        market_update_account_subscription_ = bus_.subscribe(
            consumer("execution-state-market-data-account", MessageSubjects::MARKET_DATA_UPDATED),
            [this](const BusMessage& message) {
                return onMarketDataUpdatedForAccountSnapshot(message);
            }
        );

        // Reconciliation is an explicit request/response boundary now. The request is
        // durable even if the gateway/simulated exchange starts later.
        requestExchangeSnapshot();
    }

    ~ExecutionStateServiceRuntime()
    {
        bus_.close(market_update_account_subscription_);
        bus_.close(notional_plan_subscription_);
        bus_.close(decision_subscription_);
        bus_.close(exchange_event_subscription_);
        bus_.close(exchange_snapshot_subscription_);
    }

    void run()
    {
        LG_INFO("Execution state service ready (stream {}, {} strategies, initial cash {}, reconciliation required{})",
                options_.stream, options_.strategies.size(), FormatUsd(options_.initial_cash),
                options_.periodic_reconciliation ? ", re-checked hourly 01:00-23:00 UTC and at 23:50" : "");
        LG_DEBUG("Market data database {} (source {})", options_.market_data_db, options_.market_data_source);
        std::cout.flush();

        while (running.load()) {
            const auto cycleStart = std::chrono::steady_clock::now();

            // Execution truth is always serviced. Daily economic inputs are separately
            // gated by newestCompletedUtcDate(time_handler_) inside their handlers.
            bus_.poll(exchange_snapshot_subscription_, 16, 1);
            bus_.poll(exchange_event_subscription_, 64, 1);
            bus_.poll(market_update_account_subscription_, 32, 1);
            bus_.poll(decision_subscription_, 32, 1);
            bus_.poll(notional_plan_subscription_, 32, 1);
            maybeRequestPeriodicReconciliation();
            processManualRequest();

            const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
            const auto interval = std::chrono::seconds(1);
            if (elapsed < interval)
                std::this_thread::sleep_for(interval - elapsed);
        }

        LG_INFO("Shutdown requested");
        bus_.flush();
        LG_INFO("Shutdown complete");
    }

};

} // namespace


// Process entrypoint.

int main(int argc, char** argv)
{
    ServiceLogging::setup("execution-state");

    try {
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        ExecutionStateServiceRuntime runtime(parseOptions(argc, argv));
        runtime.run();
        return 0;
    }
    catch (const std::exception& error) {
        LG_ALERT("Execution state service stopped by a fatal error: {}", error.what());
        return 1;
    }
}
