#include "dashboard_state_export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <variant>

#include "logger.h"
#include "mock/decimal.h"
#include "replay_runtime.h"

using namespace Replay;
using namespace MockVenue;
using namespace VenueContracts::V1;

std::string reconciliationState(ReconciliationState state)
{
    switch (state) {
    case ReconciliationState::Pending: return "PENDING";
    case ReconciliationState::Clean: return "CLEAN";
    case ReconciliationState::Blocked: return "BLOCKED";
    }
    return "UNKNOWN";
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

namespace {

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

std::string pipelineJson(const PipelineView& view, const DashboardExportConfig& config)
{
    if (!view.available)
        return "null";
    std::ostringstream out;
    out << std::setprecision(15);
    out << "{\"date\":\"" << jsonEscape(view.date)
        << "\",\"decisionTimestamp\":" << view.decision_timestamp
        << ",\"liquidityLength\":" << config.liquidity_length
        << ",\"liquidityTopN\":" << config.liquidity_top_n
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
    const DashboardExportConfig& config,
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
    out << "\"replaySpeed\":" << config.speed << ",\n";
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
    out << "\"pipeline\":" << pipelineJson(decision.pipeline, config) << ",\n";

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

} // namespace

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
    bool append_equity)
{
    ++generation;

    if (append_equity) {
        history.push_back(dashboardDayPoint(adapter, date, ts));
        if (history.size() > MAX_DASHBOARD_EQUITY_POINTS)
            history.erase(history.begin());
    }

    atomicWrite(
        config.dashboard_state_dir / "state.json",
        dashboardStateJson(
            adapter, config, phase, date, ts,
            day_index, day_count, rows_processed,
            market, history, decision, generation));

    LG_DEBUG("Dashboard state {} published: phase {}, date {}", generation, phase, date);
    LG_DEBUG("Dashboard state {} timestamp {}", generation, ts);

    if (config.ui_delay_ms > 0)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config.ui_delay_ms));
}

