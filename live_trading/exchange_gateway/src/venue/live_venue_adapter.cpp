#include "live_venue_adapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>

#include <boost/multiprecision/cpp_dec_float.hpp>
#include <fmt/format.h>

#include "logger.h"

namespace Venue {

namespace {

constexpr double QUANTITY_EPSILON = 1e-9;

// The rate budget is exhausted; the caller defers the work to a later poll.
class RateBudgetExhausted : public std::runtime_error {
public:
    explicit RateBudgetExhausted(Endpoint endpoint)
        : std::runtime_error(std::string("Venue rate budget exhausted before ") + endpointName(endpoint))
    {}
};

// Only accepted and final states are reported to the execution services.
std::optional<ExecutionOrderStatus> reportedStatus(OrderState state)
{
    switch (state) {
        case OrderState::Open:      return ExecutionOrderStatus::Accepted;
        case OrderState::Filled:    return ExecutionOrderStatus::Filled;
        case OrderState::Cancelled: return ExecutionOrderStatus::Canceled;
        case OrderState::Rejected:  return ExecutionOrderStatus::Rejected;
        default:                    return std::nullopt;
    }
}

double roundDownToStep(double quantity, double step)
{
    return std::floor(quantity / step + QUANTITY_EPSILON) * step;
}

double roundDownStopPrice(double sourcePrice, double unitScale, double tick)
{
    // Use decimal arithmetic at the price boundary: binary 0.1 must not turn an
    // exact tick into the preceding tick, or serialize an off-tick multiplication.
    using Decimal = boost::multiprecision::cpp_dec_float_50;
    const Decimal decimalPrice(fmt::format("{}", sourcePrice));
    const Decimal decimalScale(fmt::format("{}", unitScale));
    const Decimal decimalTick(fmt::format("{}", tick));
    return (floor(decimalPrice / decimalScale / decimalTick) * decimalTick).convert_to<double>();
}

const char* sideName(OrderSide side)
{
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

} // namespace


long long technicalUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}


// ------------------------------------------------------------------ rate limiter

LiveVenueAdapter::RateLimiter::RateLimiter(RateBudget budget)
    : budget_(budget), available_(budget.capacity)
{
    if (budget.capacity <= 0 || budget.window_ms <= 0)
        throw std::invalid_argument("Venue rate budget must be positive");
}


bool LiveVenueAdapter::RateLimiter::tryTake(int cost, long long nowMs)
{
    if (last_ms_ != 0 && nowMs > last_ms_) {
        const double refill = static_cast<double>(nowMs - last_ms_) * budget_.capacity / budget_.window_ms;
        available_ = std::min<double>(budget_.capacity, available_ + refill);
    }
    last_ms_ = nowMs;
    if (available_ < cost)
        return false;
    available_ -= cost;
    return true;
}


// ------------------------------------------------------------------ setup

LiveVenueAdapter::LiveVenueAdapter(
    VenueProtocol& protocol,
    HttpTransport& transport,
    VenueJournal& journal,
    SymbolMap symbolMap,
    BusinessDay businessDay,
    UnixMsClock nowMs,
    LiveVenueOptions options
)
    : protocol_(protocol),
      transport_(transport),
      journal_(journal),
      symbol_map_(std::move(symbolMap)),
      business_day_(std::move(businessDay)),
      now_ms_(std::move(nowMs)),
      options_(std::move(options)),
      rate_limiter_(protocol.rateBudget())
{
    if (options_.account_namespace.empty())
        throw std::invalid_argument("Venue account namespace is required");
    if (options_.working_poll_interval_ms <= 0 || options_.settle_after_ms <= 0 ||
        options_.market_refresh_interval_ms <= 0 || options_.unknown_alert_after_ms <= 0 ||
        options_.mark_price_max_age_ms <= 0 || options_.account_log_interval_ms <= 0 ||
        options_.fee_lookup_after_fill_ms <= 0)
        throw std::invalid_argument("Venue adapter intervals must be positive");
    if (!(options_.max_notional_ratio > 1.0))
        throw std::invalid_argument("Venue adapter max notional ratio must be above 1");
    if (symbol_map_.venue() != protocol_.name())
        throw std::invalid_argument("Symbol map is for " + symbol_map_.venue() + ", not " + protocol_.name());
    started_at_ms_ = now_ms_();

    LG_INFO("{} adapter ready: order submission {}, account namespace {}, {} mapped symbols",
            protocol_.name(), options_.order_submission_enabled ? "ENABLED" : "disabled (read-only)",
            options_.account_namespace, symbol_map_.entries().size());
    LG_INFO("{} fills are published with commission 0; fees and funding follow from the account history "
            "every {} s", protocol_.name(), options_.account_log_interval_ms / 1000);
}


void LiveVenueAdapter::setHandlers(ExchangeGatewayHandlers handlers)
{
    handlers_ = std::move(handlers);
}


Permissions LiveVenueAdapter::verifyPermissions()
{
    const Permissions permissions = protocol_.parsePermissions(send(protocol_.permissionsRequest(), Endpoint::Permissions));
    if (permissions.withdraw)
        throw std::runtime_error("The venue key has withdrawal access; use a key without it");
    if (options_.order_submission_enabled && !permissions.trade)
        throw std::runtime_error("Order submission is enabled but the venue key cannot trade");
    LG_INFO("{} key permissions: read {}, trade {}, withdraw no",
            protocol_.name(), permissions.read ? "yes" : "no", permissions.trade ? "yes" : "no");
    return permissions;
}


HttpResponse LiveVenueAdapter::send(const HttpRequest& request, Endpoint endpoint)
{
    if (!rate_limiter_.tryTake(protocol_.cost(endpoint), now_ms_()))
        throw RateBudgetExhausted(endpoint);
    HttpResponse response = transport_.send(request);
    LG_DEBUG("{} {} -> {}", request.method, endpointName(endpoint),
             response.transport_ok ? std::to_string(response.status) : response.transport_error);
    return response;
}


void LiveVenueAdapter::loadMarkets()
{
    const std::vector<Market> markets = protocol_.parseMarkets(send(protocol_.instrumentsRequest(), Endpoint::Instruments));
    markets_by_symbol_.clear();
    int trading = 0;
    for (const Market& market : markets) {
        markets_by_symbol_[market.symbol] = market;
        trading += market.trading ? 1 : 0;
    }
    markets_loaded_at_ms_ = now_ms_();
    LG_INFO("Loaded {} {} markets ({} trading)", markets.size(), protocol_.name(), trading);

    // A mapped contract the venue no longer lists is rejected at order time; say so now.
    std::string missing;
    for (const SymbolMapping& mapping : symbol_map_.entries()) {
        if (markets_by_symbol_.find(mapping.venue_symbol) == markets_by_symbol_.end())
            missing += (missing.empty() ? "" : ", ") + mapping.venue_symbol;
    }
    if (!missing.empty())
        LG_WARN("Symbol map contracts not listed by {}: {}", protocol_.name(), missing);
}


void LiveVenueAdapter::loadMarkPricesIfDue()
{
    if (mark_prices_loaded_at_ms_ != 0 && now_ms_() - mark_prices_loaded_at_ms_ < options_.mark_price_max_age_ms)
        return;
    mark_prices_ = protocol_.parseMarkPrices(send(protocol_.tickersRequest(), Endpoint::Tickers));
    mark_prices_loaded_at_ms_ = now_ms_();
}


void LiveVenueAdapter::loadMarketsIfDue()
{
    if (markets_loaded_at_ms_ == 0 || now_ms_() - markets_loaded_at_ms_ >= options_.market_refresh_interval_ms)
        loadMarkets();
}


// ------------------------------------------------------------------ commands

void LiveVenueAdapter::submitOrder(const SubmitOrderCommand& command)
{
    const ExecutionOrder& request = command.order;
    if (request.order_id == 0 || request.coin.empty() || !std::isfinite(request.quantity) || request.quantity <= 0.0)
        throw std::invalid_argument("Invalid order for the venue adapter");

    if (const auto existing = journal_.order(request.order_id)) {
        if (existing->coin != request.coin || existing->side != request.side ||
            std::abs(existing->source_quantity - request.quantity) > QUANTITY_EPSILON ||
            existing->type != (request.isPositionStop() ? VenueOrderType::PositionStop : VenueOrderType::Market) ||
            existing->position_stop_price != request.position_stop_price)
            throw std::invalid_argument("Order id " + std::to_string(request.order_id) +
                                        " was already journaled with different terms");
        LG_INFO("Order {} is already journaled ({}); duplicate command ignored",
                request.order_id, orderStateName(existing->state));
        return;
    }

    JournalOrder order;
    order.local_order_id = request.order_id;
    order.strategy_id = request.strategy_id;
    order.client_order_id = protocol_.clientOrderId(options_.account_namespace, request.order_id);
    order.coin = request.coin;
    order.side = request.side;
    order.source_quantity = request.quantity;
    order.type = request.isPositionStop() ? VenueOrderType::PositionStop : VenueOrderType::Market;
    // A protective stop works immediately; it never waits for the next daily open.
    order.active_from = request.isPositionStop() ? request.created_at : request.active_from;
    if (std::isfinite(request.position_stop_price) && request.position_stop_price > 0.0)
        order.position_stop_price = request.position_stop_price;
    order.updated_at_ms = now_ms_();

    if (!options_.order_submission_enabled) {
        rejectLocally(order, "order submission disabled (read-only configuration)");
        return;
    }
    if (request.entry_stop_price != 0.0 || request.protective_stop_price != 0.0 || request.parent_order_id != 0) {
        rejectLocally(order, "conditional orders are not supported by the live venue adapter");
        return;
    }
    if (request.isPositionStop() && (!std::isfinite(request.position_stop_price) ||
        request.position_stop_price <= 0.0 || request.side != OrderSide::Sell || request.fill_at_reference_price)) {
        rejectLocally(order, "INVALID_POSITION_STOP: requires a positive finite price and a SELL without reference-price filling");
        return;
    }

    const SymbolMapping* mapping = symbol_map_.bySource(request.coin);
    if (mapping == nullptr) {
        rejectLocally(order, "MARKET_UNAVAILABLE: " + request.coin + " is not in the " + protocol_.name() + " symbol map");
        return;
    }

    loadMarketsIfDue();
    const auto found = markets_by_symbol_.find(mapping->venue_symbol);
    if (found == markets_by_symbol_.end()) {
        rejectLocally(order, "MARKET_UNAVAILABLE: " + protocol_.name() + " does not list " + mapping->venue_symbol);
        return;
    }
    const Market& market = found->second;
    if (request.isPositionStop() && (!std::isfinite(market.tick_size) || market.tick_size <= 0.0)) {
        rejectLocally(order, "INVALID_STOP_TICK: the instrument has no usable price tick");
        return;
    }
    if (!market.trading) {
        rejectLocally(order, "MARKET_NOT_TRADING: " + market.symbol + " " + market.unavailable_reason);
        return;
    }

    // Venue units, rounded down to the venue step: never more than requested.
    const double venueUnits = request.quantity * mapping->venue_units_per_source_unit;
    const double rounded = roundDownToStep(venueUnits, market.quantity_step);
    order.symbol = market.symbol;
    order.unit_scale = mapping->venue_units_per_source_unit;
    order.venue_quantity = rounded;
    if (rounded + QUANTITY_EPSILON < market.quantity_step) {
        rejectLocally(order, "BELOW_MINIMUM_QUANTITY: " + FormatQuantity(venueUnits) + " " + mapping->asset +
                             " is below the venue step " + FormatQuantity(market.quantity_step));
        return;
    }

    // The USD value must survive the unit conversion. Planned: source units x the price the
    // planner used. At the venue: venue units x the current venue mark price. A wrong scale
    // in the symbol map makes these differ by its factor (1000 for 1000PEPEUSDT).
    if (!std::isfinite(request.reference_price) || request.reference_price <= 0.0) {
        rejectLocally(order, "NO_REFERENCE_PRICE: the order carries no planning price to check its USD value");
        return;
    }
    loadMarkPricesIfDue();
    const auto mark = mark_prices_.find(market.symbol);
    if (mark == mark_prices_.end()) {
        rejectLocally(order, "NO_VENUE_PRICE: " + protocol_.name() + " reports no mark price for " + market.symbol);
        return;
    }
    const double plannedUsd = request.quantity * request.reference_price;
    const double venueUsd = venueUnits * mark->second;
    const double ratio = venueUsd / plannedUsd;
    if (ratio > options_.max_notional_ratio || ratio < 1.0 / options_.max_notional_ratio) {
        LG_ALERT("Order {} for {} would be worth {} at {} but was planned at {} ({:.4g}x); check the symbol map scale "
                 "for {} -> {} (x{})", order.local_order_id, request.coin, FormatUsd(venueUsd), protocol_.name(),
                 FormatUsd(plannedUsd), ratio, request.coin, market.symbol, FormatQuantity(mapping->venue_units_per_source_unit));
        rejectLocally(order, "NOTIONAL_MISMATCH: " + FormatUsd(venueUsd) + " at the venue vs " + FormatUsd(plannedUsd) +
                             " planned");
        return;
    }

    if (request.isPositionStop()) {
        const double venuePrice = request.position_stop_price / order.unit_scale;
        if (!std::isfinite(venuePrice) || venuePrice <= 0.0) {
            rejectLocally(order, "INVALID_STOP_PRICE: the converted stop price must be finite and positive");
            return;
        }
        const double roundedPrice = roundDownStopPrice(request.position_stop_price, order.unit_scale, market.tick_size);
        if (!std::isfinite(roundedPrice) || roundedPrice <= 0.0) {
            rejectLocally(order, "INVALID_STOP_PRICE: the converted stop price is unusable after rounding down to tick");
            return;
        }
        if (roundedPrice >= mark->second) {
            rejectLocally(order, "STOP_NOT_BELOW_MARK: the venue stop price must be below the current venue mark");
            return;
        }
        order.venue_stop_price = roundedPrice;
    }

    journal_.insertOrder(order);
    LG_INFO("Journaled order {}: {} {} {} on {} (requested {} {}, about {}), active from {}",
            order.local_order_id, sideName(order.side), FormatQuantity(order.venue_quantity), mapping->asset,
            order.symbol, FormatQuantity(order.source_quantity), order.coin, FormatUsd(venueUsd),
            FormatDayKey(order.active_from));
    LG_DEBUG("Order {} client order id {}", order.local_order_id, order.client_order_id);
    if (order.type == VenueOrderType::PositionStop)
        LG_INFO("Position stop {}: reduce-only SELL triggered by mark at {} USD per venue unit "
                "({} USD per source unit requested)", order.local_order_id,
                FormatQuantity(order.venue_stop_price), FormatQuantity(order.position_stop_price));
}


void LiveVenueAdapter::cancelOrder(const CancelOrderCommand& command)
{
    if (command.order_id == 0)
        throw std::invalid_argument("Invalid cancel for the venue adapter");

    auto order = journal_.order(command.order_id);
    if (!order) {
        LG_WARN("Cancel requested for order {}, which this venue adapter never received; nothing to cancel",
                command.order_id);
        return;
    }
    if (isTerminal(order->state)) {
        LG_INFO("Cancel for order {} ignored: it is already {}", command.order_id, orderStateName(order->state));
        return;
    }

    order->cancel_requested = true;
    if (order->state == OrderState::Prepared) {
        transition(*order, OrderState::Cancelled, "cancelled before it was sent");
        LG_INFO("Order {} cancelled before it was sent", command.order_id);
    }
    else {
        journal_.updateOrder(*order);
        LG_INFO("Cancel for order {} journaled; it will be sent to the venue", command.order_id);
    }
    publishPending();
}


void LiveVenueAdapter::requestSnapshot(const ExchangeSnapshotRequest& request)
{
    // Book the account history first: a USD deposit or withdrawal moves the venue balance
    // at once, but reaches our cash only through its history entry (read every few
    // minutes). Published before the snapshot, so reconciliation sees both sides of it.
    next_account_log_ms_ = 0;
    readAccountLogIfDue();
    publishPending();

    const HttpResponse accounts = send(protocol_.accountsRequest(), Endpoint::Accounts);
    const HttpResponse positions = send(protocol_.positionsRequest(), Endpoint::Positions);
    const HttpResponse openOrders = send(protocol_.openOrdersRequest(), Endpoint::OpenOrders);
    const AccountState account = protocol_.parseAccount(accounts, positions);

    // Positions are reported in source units under the source coin name. A venue symbol
    // outside the symbol map keeps its own name, so the drift stays visible.
    auto coinAndScale = [this](const std::string& symbol) -> std::pair<Coin, double> {
        const SymbolMapping* mapping = symbol_map_.byVenueSymbol(symbol);
        if (mapping == nullptr) {
            LG_WARN("Venue symbol {} is not in the symbol map; reported under its venue name", symbol);
            return {symbol, 1.0};
        }
        return {mapping->source, mapping->venue_units_per_source_unit};
    };

    ExchangeSnapshotEvent event;
    const Timestamp today = business_day_();
    event.snapshot.timestamp = today;
    // Execution-state keeps cash as on a spot account (a buy spends its value). On the
    // venue opening a perpetual position leaves the USD balance unchanged, so the balance
    // minus the open positions' entry value is the comparable number. Kraken settles
    // realized PnL into the USD balance at once (confirmed on the account history,
    // 2026-10-15); that `price` is the average entry price is still UNCONFIRMED.
    double entryValue = 0.0;
    for (const Position& position : account.positions) {
        const auto [coin, scale] = coinAndScale(position.symbol);
        event.snapshot.positions[coin] += position.signed_quantity / scale;
        entryValue += position.signed_quantity * position.entry_price;
    }
    event.snapshot.cash = account.usd_balance - entryValue;
    event.snapshot.collateral_usd = account.collateral_usd;
    for (const OpenOrder& open : protocol_.parseOpenOrders(openOrders)) {
        const auto [coin, scale] = coinAndScale(open.symbol);
        ExchangeOpenOrderSnapshot row;
        if (const auto known = findOrder(open.client_order_id, open.venue_order_id))
            row.local_order_id = known->local_order_id;
        row.exchange_order_id = open.venue_order_id;
        row.coin = coin;
        row.side = open.side;
        row.quantity = (open.filled + open.unfilled) / scale;
        row.filled_quantity = open.filled / scale;
        event.snapshot.open_orders.push_back(row);
    }

    event.metadata.schema_version = 1;
    event.metadata.message_id = protocol_.name() + "-snapshot:" + request.metadata.message_id;
    event.metadata.correlation_id = request.metadata.message_id;
    event.metadata.produced_at = today;

    LG_INFO("Venue snapshot: USD balance {}, positions entered for {}, cash as compared {}, {} positions, {} open orders, "
            "collateral after haircut {}",
            FormatUsd(account.usd_balance), FormatUsd(entryValue), FormatUsd(event.snapshot.cash),
            event.snapshot.positions.size(), event.snapshot.open_orders.size(), FormatUsd(account.collateral_usd));
    if (handlers_.on_snapshot)
        handlers_.on_snapshot(event);
}


// ------------------------------------------------------------------ polling

void LiveVenueAdapter::poll(int timeoutMs)
{
    if (timeoutMs <= 0)
        throw std::invalid_argument("Venue adapter poll timeout must be positive");

    try {
        if (!recovered_) {
            recoverInterruptedSends();
            recovered_ = true;
        }
        publishPending();
        sendDueOrders();
        sendCancels();
        checkVenue();
        readAccountLogIfDue();
        refreshDeadMansSwitch();
        publishPending();
    }
    catch (const RateBudgetExhausted& error) {
        LG_DEBUG("{}; continuing on a later poll", error.what());
    }
    catch (const ProtocolError& error) {
        LG_ERROR("Unexpected venue response, continuing on a later poll: {}", error.what());
    }
    // Other runtime errors (journal, configuration) and logic errors stop the gateway.
}


void LiveVenueAdapter::recoverInterruptedSends()
{
    for (JournalOrder order : journal_.ordersIn({OrderState::Sending})) {
        transition(order, OrderState::Unknown, "process stopped while the order was being sent");
        LG_WARN("Order {} was being sent when the gateway stopped; checking venue evidence, it will not be resent",
                order.local_order_id);
    }
}


void LiveVenueAdapter::sendDueOrders()
{
    const Timestamp today = business_day_();
    for (JournalOrder order : journal_.ordersIn({OrderState::Prepared})) {
        if (order.cancel_requested) {
            transition(order, OrderState::Cancelled, "cancelled before it was sent");
            continue;
        }
        if (order.active_from > today)
            continue; // Held until its business day.
        sendOrder(order);
    }
}


void LiveVenueAdapter::sendOrder(JournalOrder order)
{
    loadMarketsIfDue();
    const HttpRequest request = protocol_.submitRequest(
        {order.local_order_id, order.client_order_id, order.symbol, order.side, order.venue_quantity,
         order.type, order.venue_stop_price});
    if (!rate_limiter_.tryTake(protocol_.cost(Endpoint::SendOrder), now_ms_()))
        throw RateBudgetExhausted(Endpoint::SendOrder);

    // Committed before the request leaves: a crash from here on means "maybe placed".
    order.sent_at_ms = now_ms_();
    transition(order, OrderState::Sending, "sending to venue");
    const HttpResponse response = transport_.send(request);

    SubmitResult result;
    try {
        result = protocol_.parseSubmit(response);
    }
    catch (const ProtocolError& error) {
        result = {SubmitOutcome::Unknown, {}, error.what()};
    }

    last_evidence_ms_[order.local_order_id] = now_ms_();
    switch (result.outcome) {
        case SubmitOutcome::Accepted:
            order.venue_order_id = result.venue_order_id;
            transition(order, OrderState::Open, "accepted by venue");
            LG_INFO("Order {} accepted by {}: {} {} {}", order.local_order_id, protocol_.name(),
                    sideName(order.side), FormatQuantity(order.venue_quantity), order.symbol);
            LG_DEBUG("Order {} venue order id {}", order.local_order_id, order.venue_order_id);
            last_venue_check_ms_ = 0; // Look for fills on the next poll.
            break;
        case SubmitOutcome::Rejected:
            transition(order, OrderState::Rejected, result.reason);
            LG_WARN("Order {} rejected by {}: {}", order.local_order_id, protocol_.name(), result.reason);
            break;
        case SubmitOutcome::NotProcessed:
            transition(order, OrderState::Prepared, "not processed by venue: " + result.reason);
            LG_WARN("Order {} was not processed by {} ({}); it will be sent again", order.local_order_id,
                    protocol_.name(), result.reason);
            break;
        case SubmitOutcome::Unknown:
            transition(order, OrderState::Unknown, result.reason);
            LG_ERROR("Order {} outcome unknown ({}); checking venue evidence, it will not be resent",
                     order.local_order_id, result.reason);
            break;
    }
    // Report acceptance or rejection before any fill of the same poll.
    publishPending();
}


void LiveVenueAdapter::sendCancels()
{
    for (JournalOrder order : journal_.ordersIn({OrderState::Open, OrderState::Unknown})) {
        if (!order.cancel_requested)
            continue;
        const auto attempted = last_cancel_attempt_ms_.find(order.local_order_id);
        if (attempted != last_cancel_attempt_ms_.end() &&
            now_ms_() - attempted->second < options_.working_poll_interval_ms)
            continue;

        const HttpResponse response = send(protocol_.cancelRequest(order.client_order_id), Endpoint::CancelOrder);
        last_cancel_attempt_ms_[order.local_order_id] = now_ms_();
        CancelResult result;
        try {
            result = protocol_.parseCancel(response);
        }
        catch (const ProtocolError& error) {
            result = {CancelOutcome::Unknown, error.what()};
        }

        switch (result.outcome) {
            case CancelOutcome::Cancelled:
                transition(order, OrderState::Cancelled, "cancelled at venue");
                LG_INFO("Order {} cancelled at {}", order.local_order_id, protocol_.name());
                break;
            case CancelOutcome::AlreadyFilled:
                LG_INFO("Cancel for order {}: already filled; waiting for its fills", order.local_order_id);
                break;
            case CancelOutcome::NotFound:
                LG_INFO("Cancel for order {}: not listed by the venue; fills and open orders will settle it",
                        order.local_order_id);
                break;
            case CancelOutcome::Unknown:
                LG_WARN("Cancel for order {} has no reliable answer ({}); will try again",
                        order.local_order_id, result.reason);
                break;
        }
    }
}


void LiveVenueAdapter::checkVenue()
{
    const std::vector<JournalOrder> working = journal_.ordersIn({OrderState::Open, OrderState::Unknown});
    if (working.empty())
        return;
    // Accepted stop-losses rest at the venue for days: look for their fills less often.
    const bool onlyRestingStops = std::all_of(working.begin(), working.end(), [](const JournalOrder& order) {
        return order.type == VenueOrderType::PositionStop && order.state == OrderState::Open && !order.cancel_requested;
    });
    const long long interval = onlyRestingStops ? options_.resting_poll_interval_ms : options_.working_poll_interval_ms;
    if (last_venue_check_ms_ != 0 && now_ms_() - last_venue_check_ms_ < interval)
        return;
    last_venue_check_ms_ = now_ms_();

    applyFills(protocol_.parseFills(send(protocol_.fillsRequest(), Endpoint::Fills)));
    applyOpenOrders(protocol_.parseOpenOrders(send(protocol_.openOrdersRequest(), Endpoint::OpenOrders)));
}


void LiveVenueAdapter::applyFills(const std::vector<VenueFill>& fills)
{
    std::vector<VenueFill> oldestFirst = fills;
    std::stable_sort(oldestFirst.begin(), oldestFirst.end(),
                     [](const VenueFill& a, const VenueFill& b) { return a.fill_time_ms < b.fill_time_ms; });

    for (const VenueFill& fill : oldestFirst) {
        if (journal_.knowsFill(fill.venue_fill_id))
            continue;

        auto order = findOrder(fill.client_order_id, fill.venue_order_id);
        // A venue-id fallback still needs matching symbol and side; unrelated manual
        // trades must not become our fills because of inconsistent venue evidence.
        if (order && fill.client_order_id.empty() &&
            (fill.side != order->side || fill.symbol != order->symbol))
            order.reset();
        // A fill cannot precede the order. One that does means a client id was reused
        // (for example a reset journal with the same account namespace).
        constexpr long long CLOCK_TOLERANCE_MS = 60000;
        if (order && (order->sent_at_ms == 0 || fill.fill_time_ms + CLOCK_TOLERANCE_MS < order->sent_at_ms)) {
            if (journal_.recordUnattributedFill(fill))
                LG_ALERT("Venue fill with the client id of order {} predates the order (client id reused?); "
                         "recorded for reconciliation, not attributed", order->local_order_id);
            continue;
        }
        if (!order) {
            if (!journal_.recordUnattributedFill(fill))
                continue;
            if (fill.fill_time_ms + CLOCK_TOLERANCE_MS < started_at_ms_)
                LG_INFO("Pre-existing venue fill on {} from before this adapter started; recorded, not attributed",
                        fill.symbol);
            else
                LG_ALERT("Venue fill on {} ({} {} at {}, type {}) is not from an order of this system; "
                         "recorded for reconciliation, not attributed",
                         fill.symbol, sideName(fill.side), FormatQuantity(fill.quantity),
                         FormatQuantity(fill.price), fill.fill_type);
            continue;
        }

        if (fill.side != order->side || fill.symbol != order->symbol)
            LG_ALERT("Venue fill for order {} has a different side or symbol ({} {}); recorded as reported",
                     order->local_order_id, sideName(fill.side), fill.symbol);

        order->filled_venue_quantity += fill.quantity;
        if (order->venue_order_id.empty())
            order->venue_order_id = fill.venue_order_id;
        if (order->filled_venue_quantity > order->venue_quantity + QUANTITY_EPSILON)
            LG_ALERT("Order {} filled {} but only {} was sent", order->local_order_id,
                     FormatQuantity(order->filled_venue_quantity), FormatQuantity(order->venue_quantity));

        if (order->state == OrderState::Unknown || order->state == OrderState::Open) {
            const bool complete = order->filled_venue_quantity + QUANTITY_EPSILON >= order->venue_quantity;
            order->state = complete ? OrderState::Filled : OrderState::Open;
            order->reason = complete ? "filled at venue" : "partially filled at venue";
        }
        else {
            LG_WARN("Late fill for order {}, which is already {}; recorded", order->local_order_id,
                    orderStateName(order->state));
        }
        order->updated_at_ms = now_ms_();

        const JournalFill stored = journal_.recordFill(fill, *order);
        last_evidence_ms_[order->local_order_id] = now_ms_();
        // The fee appears in the account history shortly after the fill.
        next_account_log_ms_ = std::min(next_account_log_ms_, now_ms_() + options_.fee_lookup_after_fill_ms);
        LG_INFO("Venue fill for order {}: {} {} {} at {} ({})", order->local_order_id, sideName(fill.side),
                FormatQuantity(fill.quantity), fill.symbol, FormatQuantity(fill.price), fill.fill_type);
        LG_DEBUG("Venue fill id {} -> local fill {}", fill.venue_fill_id, stored.local_fill_id);
    }
}


std::optional<JournalOrder> LiveVenueAdapter::findOrder(const std::string& clientId, const std::string& venueId) const
{
    if (!clientId.empty()) {
        if (protocol_.isOurClientOrderId(options_.account_namespace, clientId))
            return journal_.orderByClientId(clientId);
        return std::nullopt;
    }
    // The REST examples omit cliOrdId. Use only an id already bound to this journal,
    // never a symbol lookup: daily replacement intentionally has two stops per symbol.
    return journal_.orderByVenueId(venueId);
}


void LiveVenueAdapter::applyOpenOrders(const std::vector<OpenOrder>& openOrders)
{
    std::map<OrderID, OpenOrder> ours;
    for (const OpenOrder& open : openOrders) {
        if (const auto known = findOrder(open.client_order_id, open.venue_order_id)) {
            ours[known->local_order_id] = open;
        }
        else if (protocol_.isOurClientOrderId(options_.account_namespace, open.client_order_id)) {
            LG_ALERT("Venue lists an open order with our client id prefix that is not journaled ({} {})",
                     open.symbol, sideName(open.side));
        }
    }

    const long long now = now_ms_();
    for (JournalOrder order : journal_.ordersIn({OrderState::Open, OrderState::Unknown})) {
        const auto listed = ours.find(order.local_order_id);
        long long lastEvidence = order.sent_at_ms;
        if (const auto seen = last_evidence_ms_.find(order.local_order_id); seen != last_evidence_ms_.end())
            lastEvidence = std::max(lastEvidence, seen->second);

        if (listed != ours.end()) {
            last_evidence_ms_[order.local_order_id] = now;
            if (order.state == OrderState::Unknown) {
                order.venue_order_id = listed->second.venue_order_id;
                transition(order, OrderState::Open, "venue lists the order");
                LG_INFO("Order {} found open at {}; outcome resolved", order.local_order_id, protocol_.name());
            }
            continue;
        }

        if (order.state == OrderState::Open && now - lastEvidence >= options_.settle_after_ms) {
            const bool partial = order.filled_venue_quantity > QUANTITY_EPSILON;
            transition(order, OrderState::Cancelled,
                       partial ? "partially filled; the venue no longer lists the remainder"
                               : "the venue no longer lists the order and reported no fills");
            LG_WARN("Order {} ended at {} with {} of {} filled", order.local_order_id, protocol_.name(),
                    FormatQuantity(order.filled_venue_quantity), FormatQuantity(order.venue_quantity));
        }
        else if (order.state == OrderState::Unknown && now - order.sent_at_ms >= options_.unknown_alert_after_ms &&
                 unknown_alerted_.insert(order.local_order_id).second) {
            LG_ALERT("Order {} outcome is still unknown: not open and no fills seen. It will not be resent; "
                     "check the venue and resolve it manually", order.local_order_id);
        }
    }
}


// Fees, funding, interest and capital flows: the venue's recent account history. Entries
// are recorded once (the journal is the de-duplication), mapped to our fills and source
// symbols, and published.
// The very first read of a journal only marks the existing history as seen: it predates
// this account state.
void LiveVenueAdapter::readAccountLogIfDue()
{
    constexpr long long WAIT_FOR_FILL_MS = 600000;
    if (now_ms_() < next_account_log_ms_)
        return;
    // Scheduled before the request: a failing read waits for the next interval and never
    // stops the order and fill work of this poll.
    next_account_log_ms_ = now_ms_() + options_.account_log_interval_ms;
    AccountLog log;
    try {
        log = protocol_.parseAccountLog(send(protocol_.accountLogRequest(), Endpoint::AccountLog));
    }
    catch (const RateBudgetExhausted& error) {
        LG_DEBUG("{}; account history read later", error.what());
        return;
    }
    catch (const ProtocolError& error) {
        LG_ERROR("Account history unreadable, trying again in {} s: {}", options_.account_log_interval_ms / 1000,
                 error.what());
        return;
    }

    for (const std::string& kind : log.unaccounted) {
        if (unaccounted_logged_.insert(kind).second)
            LG_WARN("Account history entry \"{}\" changes the USD balance but is not booked here; "
                    "reconciliation may report a cash difference", kind);
    }
    for (const std::string& kind : log.not_cash) {
        if (unaccounted_logged_.insert(kind).second)
            LG_INFO("Account history entry \"{}\" changes the collateral, not the USD cash; "
                    "the next snapshot carries the new collateral value", kind);
    }

    const bool baseline = !journal_.accountLogBaselined();
    int recorded = 0;
    for (const CashEntry& entry : log.entries) {
        if (journal_.knowsCashEntry(entry.entry_id))
            continue;
        // A fee listed before its fill was polled: book it once the fill is known, so it is
        // attached to it (after 10 minutes it is booked on the account anyway).
        if (!baseline && !entry.venue_fill_id.empty() && !journal_.knowsFill(entry.venue_fill_id) &&
            now_ms_() - entry.time_ms < WAIT_FOR_FILL_MS) {
            next_account_log_ms_ = std::min(next_account_log_ms_, now_ms_() + options_.fee_lookup_after_fill_ms);
            continue;
        }
        JournalCashEntry stored;
        stored.entry_id = entry.entry_id;
        stored.kind = entry.kind;
        stored.day = entry.day;
        stored.amount_usd = entry.amount_usd;
        stored.published = baseline;
        stored.coin = entry.symbol;
        if (const SymbolMapping* mapping = symbol_map_.byVenueSymbol(entry.symbol))
            stored.coin = mapping->source;
        if (!entry.venue_fill_id.empty()) {
            if (const auto fill = journal_.fillByVenueId(entry.venue_fill_id)) {
                stored.local_fill_id = fill->local_fill_id;
                if (const auto order = journal_.order(fill->local_order_id))
                    stored.coin = order->coin;
            }
        }
        journal_.recordCashEntry(stored);
        ++recorded;
        if (!baseline)
            LG_INFO("Venue {} {}: {} on {}{}", venueCostKindLabel(entry.kind),
                    entry.entry_id, FormatUsd(entry.amount_usd), stored.coin.empty() ? "the account" : stored.coin,
                    stored.local_fill_id != 0 ? " (fill " + std::to_string(stored.local_fill_id) + ")" : "");
    }
    if (baseline) {
        journal_.markAccountLogBaselined();
        LG_INFO("Account history baseline: {} earlier cash entries marked as seen, not booked", recorded);
    }
}


void LiveVenueAdapter::refreshDeadMansSwitch()
{
    if (!options_.order_submission_enabled || options_.dead_mans_switch_seconds <= 0)
        return;
    if (journal_.ordersIn({OrderState::Open, OrderState::Unknown, OrderState::Sending}).empty())
        return;
    const long long interval = options_.dead_mans_switch_seconds * 1000LL / 3;
    if (now_ms_() - last_dead_mans_switch_ms_ < interval)
        return;

    const auto request = protocol_.deadMansSwitchRequest(options_.dead_mans_switch_seconds);
    if (!request)
        return;
    const HttpResponse response = send(*request, Endpoint::DeadMansSwitch);
    last_dead_mans_switch_ms_ = now_ms_();
    if (!response.transport_ok || response.status != 200)
        LG_WARN("Dead man's switch refresh failed; open orders may be cancelled by the venue");
}


// ------------------------------------------------------------------ events

void LiveVenueAdapter::transition(JournalOrder& order, OrderState state, const std::string& reason)
{
    order.state = state;
    order.reason = reason;
    order.updated_at_ms = now_ms_();
    journal_.updateOrder(order);
}


void LiveVenueAdapter::rejectLocally(JournalOrder order, const std::string& reason)
{
    order.state = OrderState::Rejected;
    order.reason = reason;
    if (journal_.order(order.local_order_id))
        journal_.updateOrder(order);
    else
        journal_.insertOrder(order);
    LG_WARN("Order {} ({} {} {}) rejected before reaching the venue: {}", order.local_order_id,
            sideName(order.side), FormatQuantity(order.source_quantity), order.coin, reason);
    publishPending();
}


void LiveVenueAdapter::publishPending()
{
    if (!handlers_.on_fill || !handlers_.on_order_update)
        return;

    // Fills before final order states, so the execution service sees quantities first.
    for (const JournalFill& fill : journal_.unpublishedFills()) {
        const auto order = journal_.order(fill.local_order_id);
        if (!order)
            throw std::runtime_error("Venue journal fill without its order");
        publishFill(fill, *order);
        journal_.markFillPublished(fill.venue_fill_id);
    }
    // Costs after fills, so a fee never arrives before its fill.
    if (handlers_.on_cost) {
        for (const JournalCashEntry& entry : journal_.unpublishedCashEntries()) {
            publishCashEntry(entry);
            journal_.markCashEntryPublished(entry.entry_id);
        }
    }
    for (const JournalOrder& order : journal_.unpublishedOrders()) {
        if (!reportedStatus(order.state))
            continue;
        publishOrder(order);
        journal_.markOrderPublished(order.local_order_id, order.state);
    }
}


void LiveVenueAdapter::publishOrder(const JournalOrder& order)
{
    const Timestamp today = business_day_();
    OrderUpdateEvent event;
    event.metadata.schema_version = 1;
    // Deterministic ids: a re-publication after a crash is a duplicate, not a new event.
    event.metadata.message_id = protocol_.name() + "-order:" + std::to_string(order.local_order_id) + ":" +
                                orderStateName(order.state);
    event.metadata.correlation_id = order.client_order_id;
    event.metadata.produced_at = today;
    event.update = OrderUpdate(order.local_order_id, today, *reportedStatus(order.state),
                               order.venue_order_id, order.reason);
    handlers_.on_order_update(event);
}


void LiveVenueAdapter::publishFill(const JournalFill& fill, const JournalOrder& order)
{
    FillEvent event;
    event.metadata.schema_version = 1;
    event.metadata.message_id = protocol_.name() + "-fill:" + fill.venue_fill_id;
    event.metadata.correlation_id = order.client_order_id;
    event.metadata.produced_at = fill.day;

    // Back to source units: quantity x price (USD notional) is unchanged by the scaling.
    event.fill.fill_id = fill.local_fill_id;
    event.fill.order_id = order.local_order_id;
    event.fill.strategy_id = order.strategy_id;
    event.fill.timestamp = fill.day;
    event.fill.coin = order.coin;
    event.fill.side = order.side;
    event.fill.quantity = fill.venue_quantity / order.unit_scale;
    event.fill.price = fill.price * order.unit_scale;
    event.fill.commission = 0.0; // Not reported by the venue fill; the fee follows as a cost.
    event.fill.validate();
    handlers_.on_fill(event);
}


void LiveVenueAdapter::publishCashEntry(const JournalCashEntry& entry)
{
    VenueCostEvent event;
    event.metadata.schema_version = 1;
    event.metadata.message_id = protocol_.name() + "-cost:" + entry.entry_id;
    event.metadata.correlation_id = entry.entry_id;
    event.metadata.produced_at = entry.day;
    event.cost_id = protocol_.name() + ":" + entry.entry_id;
    event.kind = entry.kind;
    event.timestamp = entry.day;
    event.coin = entry.coin;
    event.fill_id = entry.local_fill_id;
    event.amount_usd = entry.amount_usd;
    handlers_.on_cost(event);
}

} // namespace Venue
