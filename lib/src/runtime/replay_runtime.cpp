// Canonical historical workflow: completed close -> signals/risk -> USD plan,
// then a later execution open -> venue commands -> fill/accounting/reconciliation.
// RealTest parity maintains a separate double-precision research account view.

#include "replay_runtime.h"

namespace Replay {

ReplayRuntime::ReplayRuntime(
    StrategySignalEngine& strategy_signal,
    PortfolioRiskEngine& portfolio_risk,
    NotionalOrderPlanner& order_planner,
    std::vector<StrategyID> strategy_ids,
    MockVenue::MockExchange& mock_adapter,
    bool preserve_decimal_price_roundtrip,
    bool resolve_realtest_target_at_execution_open)
    : strategy_signal_(strategy_signal),
      portfolio_risk_(portfolio_risk),
      order_planner_(order_planner),
      strategy_ids_(std::move(strategy_ids)),
      mock_adapter_(mock_adapter),
      venue_adapter_(mock_adapter),
      preserve_decimal_price_roundtrip_(preserve_decimal_price_roundtrip),
      resolve_realtest_target_at_execution_open_(resolve_realtest_target_at_execution_open)
{
    if (strategy_ids_.empty())
        throw std::invalid_argument("Step56 requires at least one configured strategy");

    std::sort(strategy_ids_.begin(), strategy_ids_.end());
    if (std::adjacent_find(strategy_ids_.begin(), strategy_ids_.end()) != strategy_ids_.end())
        throw std::invalid_argument("Step56 strategy ids must be unique");

    for (const StrategyID id : strategy_ids_)
        strategy_positions_.emplace(
            id,
            VirtualPositionState{});

    if (resolve_realtest_target_at_execution_open_)
        parity_cash_ = std::stod(mock_adapter_.chaos().runtime().account().config().initial_cash);

    const auto caps = venue_adapter_.capabilities();

    if (!caps.supportsAll({
            VenueContracts::V1::Capability::SubmitOrder,
            VenueContracts::V1::Capability::CancelOrder,
            VenueContracts::V1::Capability::ClientOrderId,
            VenueContracts::V1::Capability::IdempotentSubmit,
            VenueContracts::V1::Capability::TimeInForceGtc,
            VenueContracts::V1::Capability::AccountSnapshot,
            VenueContracts::V1::Capability::OpenOrders,
            VenueContracts::V1::Capability::Fills,
            VenueContracts::V1::Capability::UserStream})) {
        throw std::invalid_argument("Step56 configured venue lacks required capabilities");
    }

    venue_adapter_.setEventHandler(
        [this](const VenueContracts::V1::Event& event) {
            onVenueEvent(event);
        });
}

void ReplayRuntime::onClosedSlice(const MarketSliceSnapshot& slice)
{
    if (slice.timestamp == 0 || slice.bars.empty())
        throw std::invalid_argument(
            "Step56 closed slice must be non-empty with non-zero timestamp");

    if (last_closed_timestamp_ != 0U && slice.timestamp <= last_closed_timestamp_)
        throw std::logic_error("Step56 closed slices must be strictly increasing");

    // Match already-admitted orders before exposing close(T) to Strategy.
    // Sorting makes cross-asset bar delivery deterministic.
    std::vector<MarketBarSnapshot> bars = slice.bars;
    std::sort(
        bars.begin(),
        bars.end(),
        [](const MarketBarSnapshot& a,
           const MarketBarSnapshot& b) {
            return a.coin < b.coin;
        });

    for (const auto& value : bars) {
        MockVenue::MarketBarObservation observation;
        observation.canonical_asset = value.coin;
        observation.event_time = slice.timestamp;
        observation.bar = value.bar;

        const auto result = mock_adapter_.processMarketBar(observation);

        if (result.status == MockVenue::ChaosStatus::VenueUnavailable ||
            result.status == MockVenue::ChaosStatus::OutOfOrderRejected)
            route_safe_ = false;
    }

    markCurrentPositionsAtClose(slice.timestamp, bars);

    const bool appended = rolling_market_.append(slice);
    if (!appended)
        throw std::logic_error("Step56 unexpected duplicate closed slice");

    const StrategyIntentBatch signals = strategy_signal_.onBarClose(
            rolling_market_.rawData(),
            rolling_market_.marketData(),
            slice.timestamp);

    last_signal_batch_ = signals;
    ++evidence_.signal_batches;

    const ::AccountSnapshot risk_account = portfolioRiskAccountView(slice.timestamp);

    const DecisionBatch decisions = portfolio_risk_.onSignals(
            signals,
            rolling_market_.marketData(),
            risk_account);

    ++evidence_.decision_batches;

    const ExecutionReferencePrices closes = rolling_market_.closePrices(slice.timestamp);

    ++planning_revision_;
    if (planning_revision_ == 0U)
        ++planning_revision_;

    pending_decision_timestamp_ = slice.timestamp;

    pending_plan_ = order_planner_.createPlan(
            strategy_ids_,
            strategy_positions_,
            planning_order_manager_,
            slice.timestamp,
            planning_revision_,
            closes,
            decisions,
            next_order_id_);

    next_order_id_ = pending_plan_->next_order_id;

    ++evidence_.planning_batches;
    evidence_.planned_submits += pending_plan_->submit_orders.size();
    evidence_.planned_cancels += pending_plan_->cancel_order_ids.size();

    last_closed_timestamp_ = slice.timestamp;
    ++evidence_.closed_slices;
}

void ReplayRuntime::onExecutionOpen(
    Timestamp execution_timestamp,
    const ExecutionReferencePrices& open_prices)
{
    if (execution_timestamp == 0U)
        throw std::invalid_argument("Step56 execution timestamp must be non-zero");

    if (last_execution_timestamp_ != 0U && execution_timestamp <= last_execution_timestamp_)
        throw std::logic_error("Step56 execution opens must be strictly increasing");

    if (!pending_plan_) {
        last_execution_timestamp_ = execution_timestamp;
        ++evidence_.execution_opens;
        return;
    }

    if (execution_timestamp <= pending_decision_timestamp_)
        throw std::logic_error("Step56 requires execution open after decision close");

    if (!pending_plan_->cancel_order_ids.empty())
        routeCancelBatch(*pending_plan_, execution_timestamp);

    if (!pending_plan_->submit_orders.empty()) {
        const auto reconciliation = mock_adapter_.reconcile(execution_timestamp);

        if (reconciliation.clean())
            ++evidence_.clean_reconciliations;
        else
            ++evidence_.blocked_or_pending_reconciliations;

        if (!reconciliation.clean() ||
            !mock_adapter_.canRouteNewSubmit()) {
            route_safe_ = false;
            rejectPlannedSubmitsLocally(
                *pending_plan_,
                execution_timestamp,
                "STEP56_RECONCILIATION_GATE_BLOCKED");
        } else {
            routeSubmitBatch(*pending_plan_, execution_timestamp, open_prices);
        }
    }

    pending_plan_.reset();
    pending_decision_timestamp_ = 0U;
    last_execution_timestamp_ = execution_timestamp;
    ++evidence_.execution_opens;
}

bool ReplayRuntime::hasPendingPlan() const
{
    return pending_plan_.has_value();
}

const NotionalOrderPlannerResult&
ReplayRuntime::pendingPlan() const
{
    if (!pending_plan_)
        throw std::logic_error("Step56 has no pending plan");
    return *pending_plan_;
}

const StrategyPositionSnapshot&
ReplayRuntime::strategyPositions() const
{
    return strategy_positions_;
}

const std::vector<::Fill>& ReplayRuntime::parityFills() const
{
    return parity_fills_;
}

const OrderManager&
ReplayRuntime::planningOrderManager() const
{
    return planning_order_manager_;
}

bool ReplayRuntime::routeSafe() const
{
    return route_safe_ &&
           mock_adapter_.chaos().runtime().recoverySafe();
}

ReplayEvidence ReplayRuntime::finalizeEvidence(Timestamp snapshot_time)
{
    const auto reconciliation = mock_adapter_.reconcile(snapshot_time);

    if (reconciliation.clean())
        ++evidence_.clean_reconciliations;
    else
        ++evidence_.blocked_or_pending_reconciliations;

    MockVenue::MockReconciliation
        reconciler(mock_adapter_.chaos().runtime().account().config().fee_ppm);
    const auto local = reconciler.buildLocalExpected(mock_adapter_.chaos().runtime().stream());

    evidence_.economic_fingerprint = mock_adapter_.chaos().runtime().account().
            economicFingerprint();
    evidence_.stream_fingerprint = mock_adapter_.chaos().runtime().streamFingerprint();
    evidence_.ledger_head_hash = local.ledger.head_hash;
    evidence_.chaos_evidence_fingerprint = mock_adapter_.chaos().evidenceFingerprint();

    std::ostringstream payload;
    payload
        << evidence_.closed_slices << '|'
        << evidence_.execution_opens << '|'
        << evidence_.signal_batches << '|'
        << evidence_.decision_batches << '|'
        << evidence_.planning_batches << '|'
        << evidence_.planned_submits << '|'
        << evidence_.planned_cancels << '|'
        << evidence_.canonical_fills << '|'
        << evidence_.canonical_accounting_events << '|'
        << evidence_.clean_reconciliations << '|'
        << evidence_.blocked_or_pending_reconciliations << '|'
        << evidence_.economic_fingerprint << '|'
        << evidence_.stream_fingerprint << '|'
        << evidence_.ledger_head_hash << '|'
        << evidence_.chaos_evidence_fingerprint << '|'
        << kStep49VenueFingerprint << '|'
        << kStep50LifecycleFingerprint << '|'
        << kStep51MatchingFingerprint << '|'
        << kStep52AccountingFingerprint << '|'
        << kStep53RecoveryFingerprint << '|'
        << kStep54ReconciliationLedgerFingerprint << '|'
        << kStep55FaultChaosFingerprint << '|'
        << kStep56RuntimeFingerprint;

    evidence_.full_run_fingerprint = MockVenue::Sha256::hexDigest(payload.str());

    return evidence_;
}

// Checkpoint at a completed-close boundary with no fill awaiting parity conversion.
// The next-open plan may remain pending and must survive the restart.
ReplayResumeState ReplayRuntime::snapshotResumeState() const
{
    if (!resolve_realtest_target_at_execution_open_)
        throw std::logic_error("Replay resume state is currently defined only for realtest-parity");
    if (!parity_expected_fills_.empty())
        throw std::logic_error(
            "Replay resume checkpoint requires no fill waiting at the execution edge");
    if (!last_signal_batch_.has_value() || last_closed_timestamp_ == 0U)
        throw std::logic_error("Replay resume checkpoint requires at least one completed close");

    ReplayResumeState state;
    state.rolling_market = rolling_market_;
    state.last_signal_batch = last_signal_batch_;
    state.strategy_positions = strategy_positions_;
    state.processed_fill_ids = planning_order_manager_.processedFillIds();
    state.pending_plan = pending_plan_;
    state.next_order_id = next_order_id_;
    state.planning_revision = planning_revision_;
    state.last_closed_timestamp = last_closed_timestamp_;
    state.pending_decision_timestamp = pending_decision_timestamp_;
    state.last_execution_timestamp = last_execution_timestamp_;
    state.route_safe = route_safe_;
    state.parity_cash = parity_cash_;
    state.parity_fills = parity_fills_;
    state.evidence = evidence_;

    state.planning_orders.reserve(planning_order_manager_.orders().size());
    for (const auto& [order_id, tracked] :
         planning_order_manager_.orders()) {
        (void)order_id;
        state.planning_orders.push_back(tracked);
    }
    return state;
}

void ReplayRuntime::restoreResumeState(const ReplayResumeState& state)
{
    if (!resolve_realtest_target_at_execution_open_)
        throw std::logic_error("Replay resume state is currently defined only for realtest-parity");
    if (!state.last_signal_batch.has_value() ||
        state.last_closed_timestamp == 0U ||
        state.last_signal_batch->timestamp != state.last_closed_timestamp)
        throw std::invalid_argument(
            "Replay resume state has no valid completed-close signal checkpoint");

    rolling_market_ = state.rolling_market;
    strategy_signal_.restore(*state.last_signal_batch);
    portfolio_risk_.restoreLastTimestamp(state.last_closed_timestamp);
    strategy_positions_ = state.strategy_positions;
    planning_order_manager_.restore(state.planning_orders, state.processed_fill_ids);
    pending_plan_ = state.pending_plan;
    next_order_id_ = state.next_order_id;
    planning_revision_ = state.planning_revision;
    last_closed_timestamp_ = state.last_closed_timestamp;
    pending_decision_timestamp_ = state.pending_decision_timestamp;
    last_execution_timestamp_ = state.last_execution_timestamp;
    route_safe_ = state.route_safe;
    parity_cash_ = state.parity_cash;
    parity_fills_ = state.parity_fills;
    evidence_ = state.evidence;

    parity_expected_fills_.clear();
    request_items_.clear();
}

std::uint64_t ReplayRuntime::fillIdHash(const std::string& native_fill_id)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch :
         native_fill_id) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

double ReplayRuntime::unitsToDouble(std::uint64_t units, std::size_t scale)
{
    long double factor = 1.0L;
    for (std::size_t i = 0U; i < scale; ++i)
        factor *= 10.0L;

    return static_cast<double>(static_cast<long double>(units) / factor);
}

// Preserve the historical double division path used by exact decimal prices.
// A long-double round trip can otherwise shift the final binary price by one ULP.
double ReplayRuntime::decimalGridUnitsToDouble(std::uint64_t units, std::size_t scale)
{
    double factor = 1.0;
    for (std::size_t i = 0U; i < scale; ++i)
        factor *= 10.0;
    return static_cast<double>(units) / factor;
}

std::uint64_t ReplayRuntime::quantizePriceFloor(double value, std::size_t scale) const
{
    if (preserve_decimal_price_roundtrip_) {
        std::uint64_t exact_units = 0U;
        if (MockVenue::doubleToScaledUnitsExact(value, scale, &exact_units))
            return exact_units;
    }
    return quantizeFloor(value, scale);
}

std::uint64_t ReplayRuntime::quantizePriceCeil(double value, std::size_t scale) const
{
    if (preserve_decimal_price_roundtrip_) {
        std::uint64_t exact_units = 0U;
        if (MockVenue::doubleToScaledUnitsExact(value, scale, &exact_units))
            return exact_units;
    }
    return quantizeCeil(value, scale);
}

double ReplayRuntime::priceUnitsToDouble(std::uint64_t units, std::size_t scale) const
{
    return preserve_decimal_price_roundtrip_
        ? decimalGridUnitsToDouble(units, scale)
        : unitsToDouble(units, scale);
}

std::uint64_t ReplayRuntime::quantizeFloor(double value, std::size_t scale)
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0U;

    long double factor = 1.0L;
    for (std::size_t i = 0U; i < scale; ++i)
        factor *= 10.0L;

    const long double scaled = static_cast<long double>(value) *
        factor;

    if (scaled <= 0.0L ||
        scaled >
            static_cast<long double>(
                std::numeric_limits<
                    std::uint64_t>::max()))
        return 0U;

    return static_cast<std::uint64_t>(std::floor(scaled + 1e-12L));
}

std::uint64_t ReplayRuntime::quantizeCeil(double value, std::size_t scale)
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0U;

    long double factor = 1.0L;
    for (std::size_t i = 0U; i < scale; ++i)
        factor *= 10.0L;

    const long double scaled = static_cast<long double>(value) *
        factor;

    if (scaled <= 0.0L ||
        scaled >
            static_cast<long double>(
                std::numeric_limits<
                    std::uint64_t>::max()))
        return 0U;

    return static_cast<std::uint64_t>(std::ceil(scaled - 1e-12L));
}

::AccountSnapshot ReplayRuntime::portfolioRiskAccountView(Timestamp timestamp) const
{
    const auto canonical = mock_adapter_.chaos().runtime().account().accountSnapshot(timestamp);

    ::AccountSnapshot view;
    view.timestamp = timestamp;

    // RealTest-parity mirrors the historical Backtester account exactly:
    // double cash updated by fills, plus exact filled quantities marked at close.
    // The canonical MOCK account still runs independently for venue/reconciliation
    // evidence, but its fixed-point/grid rounding must not alter research sizing.
    if (resolve_realtest_target_at_execution_open_) {
        view.cash = parity_cash_;
        for (const auto& [strategy_id, positions] : strategy_positions_) {
            (void)strategy_id;
            for (const auto& [coin, quantity] : positions.values())
                view.positions[coin] += quantity;
        }
    } else {
        // Normal MOCK uses the canonical perpetual-account view.
        view.cash = canonical.equity;
    }

    for (const StrategyID id :
         strategy_ids_) {
        const auto it = strategy_positions_.find(id);

        std::unordered_map<Coin, double>
            positions;

        if (it != strategy_positions_.end())
            positions = it->second.values();

        view.strategy_positions.emplace(id, std::move(positions));
    }

    return view;
}

void ReplayRuntime::markCurrentPositionsAtClose(
    Timestamp timestamp,
    const std::vector<MarketBarSnapshot>& bars)
{
    std::map<std::string, double> closes;
    for (const auto& value : bars)
        closes.emplace(value.coin, value.bar.close);

    const auto snapshot = mock_adapter_.chaos().runtime().account().accountSnapshot(timestamp);

    for (const auto& position :
         snapshot.positions) {
        const auto it = closes.find(position.instrument.market.canonical_asset);
        if (it == closes.end())
            throw std::runtime_error(
                "Step56 missing close for held MOCK position: asset=" +
                position.instrument.market.canonical_asset +
                " timestamp=" + std::to_string(timestamp));

        const auto result = mock_adapter_.markToMarket(
                position.instrument.market.canonical_asset,
                timestamp,
                it->second);

        if (result != MockVenue::RecoverySourceResult::Applied &&
            result != MockVenue::RecoverySourceResult::DuplicateIgnored)
            throw std::runtime_error("Step56 canonical mark was rejected");
    }
}

void ReplayRuntime::routeCancelBatch(
    const NotionalOrderPlannerResult& plan,
    Timestamp execution_timestamp)
{
    VenueContracts::V1::CancelOrderBatch
        batch;
    batch.request = makeRequest(
            "replay-cancel:" +
            std::to_string(last_closed_timestamp_) + ":" +
            std::to_string(execution_timestamp),
            execution_timestamp);

    for (const OrderID id :
         plan.cancel_order_ids) {
        const auto* stored = mock_adapter_.chaos().runtime().lifecycle().findOrder(id);

        if (stored == nullptr || !stored->active())
            continue;

        VenueContracts::V1::CancelOrderItem item;
        item.item_id = "order:" + std::to_string(id);
        item.order.local_order_id = id;
        item.order.instrument = stored->intent.instrument;
        item.order.native_references = stored->native_references;
        batch.items.push_back(std::move(item));
    }

    if (batch.items.empty())
        return;

    rememberRequestItems(batch);
    venue_adapter_.cancelOrders(batch);
}

void ReplayRuntime::routeSubmitBatch(
    const NotionalOrderPlannerResult& plan,
    Timestamp execution_timestamp,
    const ExecutionReferencePrices&
        open_prices)
{
    VenueContracts::V1::SubmitOrderBatch
        batch;

    batch.request = makeRequest(
            "replay-submit:" +
            std::to_string(plan.submit_orders.front().decision_timestamp) + ":" +
            std::to_string(execution_timestamp),
            execution_timestamp);

    std::vector<ExecutionOrder>
        mirror_orders;

    for (const auto& planned :
         plan.submit_orders) {
        const auto canonical = canonicalOrder(planned, execution_timestamp, open_prices);

        VenueContracts::V1::SubmitOrderItem item;
        item.item_id = "order:" +
            std::to_string(planned.order_id);
        item.order = canonical;
        batch.items.push_back(item);

        double mirror_quantity = canonical.quantity;
        if (resolve_realtest_target_at_execution_open_) {
            mirror_quantity = realtestParityOrderQuantity(planned, open_prices);
            parity_expected_fills_[planned.order_id] = ParityExpectedFill{
                mirror_quantity,
                open_prices.get(planned.coin),
                planned.side
            };
        }

        mirror_orders.emplace_back(
            planned.order_id,
            planned.strategy_id,
            planned.created_at,
            execution_timestamp,
            planned.coin,
            planned.side == OrderSide::Buy
                ? OrderSide::Buy
                : OrderSide::Sell,
            mirror_quantity);
    }

    rememberRequestItems(batch);

    // The compatibility OrderManager is a planner mirror only.
    // It is tracked before the synchronous adapter call so canonical
    // events emitted inside the call can be consumed immediately.
    for (const auto& order :
         mirror_orders) {
        planning_order_manager_.track(order);
        planning_order_manager_.markSubmitted(order.order_id, execution_timestamp);
    }

    venue_adapter_.submitOrders(batch);
}

// Use the close-approved USD target divided by the next open, then subtract
// filled quantity. A target of zero closes the exact holding instead of re-sizing it.
double ReplayRuntime::realtestParityOrderQuantity(
    const PlannedNotionalOrder& planned,
    const ExecutionReferencePrices& open_prices) const
{
    const double open = open_prices.get(planned.coin);
    if (!std::isfinite(open) || open <= 0.0)
        throw std::runtime_error("invalid RealTest-parity open price");

    const auto strategy_it = strategy_positions_.find(planned.strategy_id);
    const double current_quantity = strategy_it == strategy_positions_.end()
        ? 0.0
        : strategy_it->second.get(planned.coin);

    if (planned.target_notional_usd == 0.0)
        return std::abs(current_quantity);

    const double target_quantity = planned.target_notional_usd / open;
    return std::abs(target_quantity - current_quantity);
}

VenueContracts::V1::LimitOrderIntent
ReplayRuntime::canonicalOrder(
    const PlannedNotionalOrder& planned,
    Timestamp execution_timestamp,
    const ExecutionReferencePrices&
        open_prices) const
{
    const auto* entry = MockVenue::findByCanonicalAssetExact(planned.coin);

    if (entry == nullptr || !entry->enabled)
        throw std::runtime_error("Step56 UNMAPPED/DISABLED asset: " + planned.coin);

    const auto* rules = MockVenue::rulesForExact(*entry);

    if (rules == nullptr || !rules->valid())
        throw std::runtime_error("Step56 missing MOCK trading rules");

    if (!open_prices.contains(planned.coin))
        throw std::runtime_error("Step56 missing open(T+1) for planned asset");

    if (!std::isfinite(
            planned.reference_close) ||
        planned.reference_close <= 0.0 ||
        !std::isfinite(planned.notional_usd) ||
        planned.notional_usd <= 0.0)
        throw std::runtime_error("Step56 invalid planner notional edge input");

    // The historical Backtester decides the monetary target at close(T),
    // but resolves that target into an executable asset quantity only when
    // OPEN(T+1) is known.  RealTest parity reproduces that exact boundary:
    // strategy/risk own the close-based USD target; execution owns the
    // open-based quantity conversion. Normal MOCK keeps the live-oriented
    // close-reference conversion used by the canonical venue path.
    const double open = open_prices.get(planned.coin);
    if (!std::isfinite(open) || open <= 0.0)
        throw std::runtime_error("Step56 invalid open(T+1) for planned asset");

    double raw_quantity = planned.notional_usd /
        planned.reference_close;

    if (resolve_realtest_target_at_execution_open_)
        raw_quantity = realtestParityOrderQuantity(planned, open_prices);

    std::uint64_t quantity_units = 0U;

    // Exact FLAT must not leave a one-tick residual because of
    // notional->double->quantity reconstruction. Strategy positions are
    // themselves canonical Fill quantities already on the venue grid.
    if (planned.target_notional_usd == 0.0 &&
        !resolve_realtest_target_at_execution_open_) {
        const auto strategy_it = strategy_positions_.find(planned.strategy_id);

        const double current_quantity = strategy_it == strategy_positions_.end()
            ? 0.0
            : strategy_it->second.get(planned.coin);

        if (current_quantity != 0.0) {
            if (!MockVenue::doubleToScaledUnitsExact(
                        std::abs(current_quantity),
                        rules->size_scale,
                        &quantity_units))
                throw std::runtime_error("Step56 exact FLAT quantity left MOCK size grid");
        }
    }

    if (quantity_units == 0U)
        quantity_units = quantizeFloor(raw_quantity, rules->size_scale);

    if (quantity_units == 0U)
        throw std::runtime_error("Step56 venue quantity quantized to zero");

    const std::uint64_t price_units = planned.side == OrderSide::Buy
        ? quantizePriceCeil(open, rules->price_scale)
        : quantizePriceFloor(open, rules->price_scale);

    if (price_units == 0U)
        throw std::runtime_error("Step56 execution price quantized to zero");

    VenueContracts::V1::LimitOrderIntent order;
    order.local_order_id = planned.order_id;
    order.strategy_id = planned.strategy_id;
    order.created_at = planned.created_at;
    order.active_from = execution_timestamp;
    order.instrument = entry->instrument();
    order.side = planned.side == OrderSide::Buy
        ? VenueContracts::V1::Side::Buy
        : VenueContracts::V1::Side::Sell;
    order.quantity = unitsToDouble(quantity_units, rules->size_scale);
    order.limit_price = priceUnitsToDouble(price_units, rules->price_scale);
    order.time_in_force = VenueContracts::V1::TimeInForce::Gtc;
    order.post_only = false;
    order.reduce_only = false;
    order.client_order_id = planned.economic_order_id;

    if (!order.valid())
        throw std::runtime_error("Step56 generated invalid canonical order");

    return order;
}

VenueContracts::V1::RequestIdentity
ReplayRuntime::makeRequest(const std::string& id, Timestamp timestamp) const
{
    VenueContracts::V1::RequestIdentity request;
    request.venue = venue_adapter_.context();
    request.request_id = id;
    request.correlation_id = "step56:" + id;
    request.requested_at = timestamp;
    return request;
}

void ReplayRuntime::rememberRequestItems(const VenueContracts::V1::SubmitOrderBatch& batch)
{
    auto& target = request_items_[
            batch.request.request_id];

    for (const auto& item : batch.items)
        target.emplace(item.item_id, item.order.local_order_id);
}

void ReplayRuntime::rememberRequestItems(const VenueContracts::V1::CancelOrderBatch& batch)
{
    auto& target = request_items_[
            batch.request.request_id];

    for (const auto& item : batch.items)
        target.emplace(item.item_id, item.order.local_order_id);
}

void ReplayRuntime::rejectPlannedSubmitsLocally(
    const NotionalOrderPlannerResult& plan,
    Timestamp timestamp,
    const std::string& reason)
{
    // The route was blocked before the canonical adapter command was
    // submitted, therefore these planner intents never became venue orders.
    // Do not create phantom OrderManager pending state.
    (void)plan;
    (void)timestamp;
    (void)reason;
}

void ReplayRuntime::onVenueEvent(const VenueContracts::V1::Event& event)
{
    if (std::holds_alternative<
            VenueContracts::V1::OperationResult>(event)) {
        onOperationResult(std::get< VenueContracts::V1::OperationResult>(event));
        return;
    }

    if (std::holds_alternative<
            VenueContracts::V1::OrderUpdate>(event)) {
        onOrderUpdate(std::get< VenueContracts::V1::OrderUpdate>(event));
        return;
    }

    if (std::holds_alternative<
            VenueContracts::V1::Fill>(
                event)) {
        onFill(std::get< VenueContracts::V1::Fill>(event));
        return;
    }

    if (std::holds_alternative<
            VenueContracts::V1::AccountingEvent>(event)) {
        ++evidence_.canonical_accounting_events;
    }
}

void ReplayRuntime::onOperationResult(const VenueContracts::V1::OperationResult& result)
{
    const auto request_it = request_items_.find(result.request.request_id);

    if (request_it == request_items_.end())
        return;

    if (result.scope == VenueContracts::V1::ResultScope::Operation &&
        !result.accepted) {
        for (const auto& pair : request_it->second)
            markMirrorRejected(
                pair.second,
                result.request.requested_at,
                result.operation_error.detail);
        return;
    }

    for (const auto& item :
         result.item_results) {
        if (item.accepted)
            continue;

        const auto order_it = request_it->second.find(item.item_id);
        if (order_it != request_it->second.end())
            markMirrorRejected(order_it->second, result.request.requested_at, item.error.detail);
    }
}

void ReplayRuntime::markMirrorRejected(
    OrderID order_id,
    Timestamp timestamp,
    const std::string& message)
{
    const auto* tracked = planning_order_manager_.find(order_id);
    if (tracked == nullptr || !tracked->isOpen())
        return;

    planning_order_manager_.onOrderUpdate(
            OrderUpdate(
                order_id,
                timestamp,
                ExecutionOrderStatus::Rejected,
                {},
                message));
}

void ReplayRuntime::onOrderUpdate(const VenueContracts::V1::OrderUpdate& update)
{
    const auto* tracked = planning_order_manager_.find(update.local_order_id);
    if (tracked == nullptr)
        return;

    switch (update.status) {
    case VenueContracts::V1::OrderLifecycleStatus::PendingSubmit:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::Submitted,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::Accepted:
    case VenueContracts::V1::OrderLifecycleStatus::Resting:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::Accepted,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::PartiallyFilled:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::PartiallyFilled,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::Filled:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::Filled,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::CancelPending:
        // Legacy planner mirror has no CANCEL_PENDING state.
        break;

    case VenueContracts::V1::OrderLifecycleStatus::Canceled:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::Canceled,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::Rejected:
    case VenueContracts::V1::OrderLifecycleStatus::VenueTerminated:
        planning_order_manager_.onOrderUpdate(
                OrderUpdate(
                    update.local_order_id,
                    update.timestamp,
                    ExecutionOrderStatus::Rejected,
                    update.native_references.native_order_id,
                    update.native_status));
        break;

    case VenueContracts::V1::OrderLifecycleStatus::UnknownRequiresReconciliation:
        route_safe_ = false;
        break;
    }
}

void ReplayRuntime::onFill(const VenueContracts::V1::Fill& fill)
{
    Fill legacy;
    legacy.fill_id = fillIdHash(fill.native_references.native_fill_id);
    legacy.order_id = fill.local_order_id;
    legacy.strategy_id = fill.strategy_id;
    legacy.timestamp = fill.timestamp;
    legacy.coin = fill.instrument.market.canonical_asset;
    legacy.side = fill.side == VenueContracts::V1::Side::Buy
        ? OrderSide::Buy
        : OrderSide::Sell;
    legacy.quantity = fill.quantity;
    legacy.price = fill.price;
    legacy.commission = 0.0;

    if (resolve_realtest_target_at_execution_open_) {
        // The parity profile requires full next-open fills. Its planner mirror retains
        // the Backtester quantity/price doubles; canonical grid fills still drive MOCK evidence.
        const auto expected = parity_expected_fills_.find(fill.local_order_id);
        if (expected != parity_expected_fills_.end()) {
            legacy.quantity = expected->second.quantity;
            legacy.price = expected->second.price;
            legacy.side = expected->second.side;
        }
    }

    const bool is_new = planning_order_manager_.onFill(legacy);

    if (!is_new)
        return;

    if (resolve_realtest_target_at_execution_open_) {
        parity_cash_ -= legacy.signedQuantity() * legacy.price; // Research cash follows spot-style buy/sell flows.
        parity_fills_.push_back(legacy);
        parity_expected_fills_.erase(fill.local_order_id);
    }

    const auto strategy_it = strategy_positions_.find(fill.strategy_id);
    if (strategy_it == strategy_positions_.end()) {
        route_safe_ = false;
        throw std::runtime_error("Step56 fill references unknown strategy");
    }

    strategy_it->second.add(legacy.coin, legacy.signedQuantity());

    ++evidence_.canonical_fills;
}

} // namespace Replay
