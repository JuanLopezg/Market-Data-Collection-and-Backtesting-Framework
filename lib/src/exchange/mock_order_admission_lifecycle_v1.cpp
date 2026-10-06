#include "mock_order_admission_lifecycle_v1.h"

namespace MockVenueV1 {

bool MockOrderAdmissionLifecycleV1::applyExternalLifecycleTransition(
    OrderID local_order_id,
    Timestamp timestamp,
    OrderLifecycleStatus next_status,
    double cumulative_filled_quantity,
    double remaining_quantity,
    const std::string& native_status,
    const Error& terminal_reason)
{
    auto it = orders_.find(local_order_id);
    if (it == orders_.end() || !validTransition(it->second.status, next_status))
        return false;

    StoredOrder& stored = it->second;
    if (!std::isfinite(cumulative_filled_quantity) ||
        cumulative_filled_quantity < 0.0 ||
        !std::isfinite(remaining_quantity) ||
        remaining_quantity < 0.0 ||
        cumulative_filled_quantity > stored.intent.quantity ||
        remaining_quantity > stored.intent.quantity)
        return false;

    if (next_status == OrderLifecycleStatus::Filled && remaining_quantity != 0.0)
        return false;

    stored.status = next_status;
    stored.cumulative_filled_quantity = cumulative_filled_quantity;
    stored.remaining_quantity = remaining_quantity;

    emit(makeOrderUpdate(
        stored, timestamp, {}, native_status, terminal_reason));
    return true;
}

Error MockOrderAdmissionLifecycleV1::noError()
{
    Error e;
    e.classification = ErrorClass::None;
    e.retryable = false;
    return e;
}

Error MockOrderAdmissionLifecycleV1::makeError(
    ErrorClass classification,
    const std::string& detail,
    const std::string& native_reason)
{
    Error e;
    e.classification = classification;
    e.retryable = false;
    e.native_code = "MOCK_V1";
    e.native_reason = native_reason;
    e.detail = detail;
    return e;
}

bool MockOrderAdmissionLifecycleV1::isMockContext(const RequestIdentity& request)
{
    return request.valid() &&
           request.venue.venue_id == "MOCK" &&
           request.venue.environment == VenueContracts::VenueEnvironment::Mock &&
           request.venue.custom_environment.empty();
}

std::string MockOrderAdmissionLifecycleV1::doubleText(double value)
{
    char buffer[128];
    const auto r = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    return r.ec == std::errc()
        ? std::string(buffer, r.ptr)
        : std::string("<invalid-double>");
}

void MockOrderAdmissionLifecycleV1::appendVenue(std::ostringstream& out, const RequestIdentity& r)
{
    out << r.venue.venue_id << '|'
        << static_cast<int>(r.venue.environment) << '|'
        << r.venue.custom_environment << '|'
        << r.request_id << '|'
        << r.correlation_id << '|'
        << r.requested_at;
}

void MockOrderAdmissionLifecycleV1::appendInstrument(std::ostringstream& out, const InstrumentIdentity& i)
{
    out << i.market.canonical_asset << '|'
        << static_cast<int>(i.market.product_class) << '|'
        << i.market.quote_or_settlement_asset << '|'
        << i.market.contract_variant << '|'
        << i.venue_symbol << '|'
        << i.venue_asset_id;
}

void MockOrderAdmissionLifecycleV1::appendOrder(std::ostringstream& out, const LimitOrderIntent& o)
{
    out << o.local_order_id << '|'
        << o.strategy_id << '|'
        << o.created_at << '|'
        << o.active_from << '|';
    appendInstrument(out, o.instrument);
    out << '|'
        << static_cast<int>(o.side) << '|'
        << doubleText(o.quantity) << '|'
        << doubleText(o.limit_price) << '|'
        << static_cast<int>(o.time_in_force) << '|'
        << (o.post_only ? 1 : 0) << '|'
        << (o.reduce_only ? 1 : 0) << '|'
        << o.client_order_id;
}

std::string MockOrderAdmissionLifecycleV1::fingerprint(const SubmitOrderBatch& b)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "SUBMIT|";
    appendVenue(out, b.request);
    for (const auto& x : b.items) {
        out << "|ITEM|" << x.item_id << '|';
        appendOrder(out, x.order);
    }
    return out.str();
}

std::string MockOrderAdmissionLifecycleV1::fingerprint(const CancelOrderBatch& b)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "CANCEL|";
    appendVenue(out, b.request);
    for (const auto& x : b.items) {
        out << "|ITEM|" << x.item_id << '|' << x.order.local_order_id << '|';
        appendInstrument(out, x.order.instrument);
        out << '|' << x.order.native_references.native_order_id << '|'
            << x.order.native_references.native_client_order_id;
    }
    return out.str();
}

std::string MockOrderAdmissionLifecycleV1::fingerprint(const ModifyOrderBatch& b)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "MODIFY|";
    appendVenue(out, b.request);
    for (const auto& x : b.items) {
        out << "|ITEM|" << x.item_id << '|' << x.order.local_order_id << '|';
        appendInstrument(out, x.order.instrument);
        out << '|' << x.order.native_references.native_order_id << '|';
        appendOrder(out, x.replacement);
    }
    return out.str();
}

OperationResult MockOrderAdmissionLifecycleV1::operationFailure(
    const RequestIdentity& request,
    CommandKind command,
    ErrorClass classification,
    const std::string& detail,
    const std::string& native_reason)
{
    OperationResult r;
    r.request = request;
    r.command = command;
    r.scope = ResultScope::Operation;
    r.accepted = false;
    r.operation_error = makeError(classification, detail, native_reason);
    return r;
}

ItemResult MockOrderAdmissionLifecycleV1::rejectedItem(
    const std::string& item_id,
    ErrorClass classification,
    const std::string& detail,
    const std::string& native_reason)
{
    ItemResult r;
    r.item_id = item_id.empty() ? "<missing-item-id>" : item_id;
    r.accepted = false;
    r.error = makeError(classification, detail, native_reason);
    return r;
}

ItemResult MockOrderAdmissionLifecycleV1::acceptedItem(
    const std::string& item_id,
    const NativeReferences& refs)
{
    ItemResult r;
    r.item_id = item_id;
    r.accepted = true;
    r.error = noError();
    r.native_references = refs;
    return r;
}

OperationResult MockOrderAdmissionLifecycleV1::processSubmitItems(const SubmitOrderBatch& batch)
{
    if (batch.items.empty())
        return operationFailure(
            batch.request, CommandKind::Submit, ErrorClass::InvalidRequest,
            "submit batch contains no items", "EMPTY_BATCH");

    OperationResult r;
    r.request = batch.request;
    r.command = CommandKind::Submit;
    r.scope = ResultScope::Item;
    r.accepted = true;
    r.operation_error = noError();

    for (const auto& item : batch.items)
        r.item_results.push_back(processSubmitItem(batch.request, item));
    return r;
}

ItemResult MockOrderAdmissionLifecycleV1::processSubmitItem(
    const RequestIdentity& request,
    const VenueContracts::V1::SubmitOrderItem& item)
{
    if (item.item_id.empty())
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "submit item_id is required", "MISSING_ITEM_ID");

    const auto& order = item.order;
    if (!order.valid())
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "canonical limit order is structurally invalid", "INVALID_ORDER");

    if (orders_.find(order.local_order_id) != orders_.end())
        return rejectedItem(
            item.item_id, ErrorClass::DuplicateRequest,
            "local_order_id has already been used", "DUPLICATE_LOCAL_ORDER_ID");

    Error admission = admitOrder(order);
    if (!admission.none())
        return rejectedItem(
            item.item_id, admission.classification,
            admission.detail, admission.native_reason);

    if (!order.client_order_id.empty() &&
        reserved_client_order_ids_.find(order.client_order_id) !=
            reserved_client_order_ids_.end())
        return rejectedItem(
            item.item_id, ErrorClass::DuplicateRequest,
            "client_order_id has already been reserved by historical order state",
            "DUPLICATE_CLIENT_ORDER_ID");

    NativeReferences refs;
    refs.native_order_id = "mock-order-" + std::to_string(order.local_order_id);
    refs.native_client_order_id = order.client_order_id;

    StoredOrder stored;
    stored.intent = order;
    stored.native_references = refs;
    stored.status = OrderLifecycleStatus::Accepted;
    stored.remaining_quantity = order.quantity;
    stored.admitted_request_id = request.request_id;

    orders_.emplace(order.local_order_id, stored);
    if (!order.client_order_id.empty())
        reserved_client_order_ids_.emplace(
            order.client_order_id, order.local_order_id);

    pending_after_result_.push_back(makeOrderUpdate(
        orders_.find(order.local_order_id)->second,
        request.requested_at,
        request.request_id,
        "ADMITTED_ACCEPTED_WAITING_MATCHER",
        noError()));

    return acceptedItem(item.item_id, refs);
}

OperationResult MockOrderAdmissionLifecycleV1::processCancelItems(const CancelOrderBatch& batch)
{
    if (batch.items.empty())
        return operationFailure(
            batch.request, CommandKind::Cancel, ErrorClass::InvalidRequest,
            "cancel batch contains no items", "EMPTY_BATCH");

    OperationResult r;
    r.request = batch.request;
    r.command = CommandKind::Cancel;
    r.scope = ResultScope::Item;
    r.accepted = true;
    r.operation_error = noError();

    for (const auto& item : batch.items)
        r.item_results.push_back(processCancelItem(batch.request, item));
    return r;
}

ItemResult MockOrderAdmissionLifecycleV1::processCancelItem(
    const RequestIdentity& request,
    const VenueContracts::V1::CancelOrderItem& item)
{
    if (!item.valid())
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "cancel item is structurally invalid", "INVALID_CANCEL");

    auto it = orders_.find(item.order.local_order_id);
    if (it == orders_.end() || !it->second.active())
        return rejectedItem(
            item.item_id, ErrorClass::OrderNotFound,
            "active order was not found", "ACTIVE_ORDER_NOT_FOUND");

    if (!sameInstrument(item.order.instrument, it->second.intent.instrument))
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "cancel instrument does not match stored order identity",
            "ORDER_IDENTITY_MISMATCH");

    if (!item.order.native_references.native_order_id.empty() &&
        item.order.native_references.native_order_id !=
            it->second.native_references.native_order_id)
        return rejectedItem(
            item.item_id, ErrorClass::OrderNotFound,
            "native_order_id does not match stored order",
            "NATIVE_ORDER_ID_MISMATCH");

    it->second.status = OrderLifecycleStatus::CancelPending;
    pending_after_result_.push_back(makeOrderUpdate(
        it->second, request.requested_at, request.request_id,
        "CANCEL_PENDING", noError()));

    it->second.status = OrderLifecycleStatus::Canceled;
    pending_after_result_.push_back(makeOrderUpdate(
        it->second, request.requested_at, request.request_id,
        "CANCELED_BY_CLIENT", noError()));

    return acceptedItem(item.item_id, it->second.native_references);
}

OperationResult MockOrderAdmissionLifecycleV1::processModifyItems(const ModifyOrderBatch& batch)
{
    if (batch.items.empty())
        return operationFailure(
            batch.request, CommandKind::Modify, ErrorClass::InvalidRequest,
            "modify batch contains no items", "EMPTY_BATCH");

    OperationResult r;
    r.request = batch.request;
    r.command = CommandKind::Modify;
    r.scope = ResultScope::Item;
    r.accepted = true;
    r.operation_error = noError();

    for (const auto& item : batch.items)
        r.item_results.push_back(processModifyItem(batch.request, item));
    return r;
}

ItemResult MockOrderAdmissionLifecycleV1::processModifyItem(
    const RequestIdentity& request,
    const VenueContracts::V1::ModifyOrderItem& item)
{
    if (!item.valid())
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "modify item is structurally invalid", "INVALID_MODIFY");

    auto it = orders_.find(item.order.local_order_id);
    if (it == orders_.end() || !it->second.active())
        return rejectedItem(
            item.item_id, ErrorClass::OrderNotFound,
            "active order was not found", "ACTIVE_ORDER_NOT_FOUND");

    if (!sameInstrument(item.order.instrument, it->second.intent.instrument) ||
        !sameInstrument(item.replacement.instrument, it->second.intent.instrument))
        return rejectedItem(
            item.item_id, ErrorClass::InvalidRequest,
            "modify may not change instrument identity", "ORDER_IDENTITY_MISMATCH");

    if (!item.order.native_references.native_order_id.empty() &&
        item.order.native_references.native_order_id !=
            it->second.native_references.native_order_id)
        return rejectedItem(
            item.item_id, ErrorClass::OrderNotFound,
            "native_order_id does not match stored order",
            "NATIVE_ORDER_ID_MISMATCH");

    Error admission = admitOrder(item.replacement);
    if (!admission.none())
        return rejectedItem(
            item.item_id, admission.classification,
            admission.detail, admission.native_reason);

    const auto& new_client = item.replacement.client_order_id;
    if (!new_client.empty()) {
        const auto existing = reserved_client_order_ids_.find(new_client);
        if (existing != reserved_client_order_ids_.end() &&
            existing->second != item.replacement.local_order_id)
            return rejectedItem(
                item.item_id, ErrorClass::DuplicateRequest,
                "replacement client_order_id belongs to another historical order",
                "DUPLICATE_CLIENT_ORDER_ID");
    }

    if (!new_client.empty())
        reserved_client_order_ids_.emplace(
            new_client, item.replacement.local_order_id);

    it->second.intent = item.replacement;
    it->second.native_references.native_client_order_id = new_client;
    it->second.remaining_quantity = std::max(
        0.0, item.replacement.quantity - it->second.cumulative_filled_quantity);

    pending_after_result_.push_back(makeOrderUpdate(
        it->second, request.requested_at, request.request_id,
        "MODIFIED_ACCEPTED", noError()));

    return acceptedItem(item.item_id, it->second.native_references);
}

bool MockOrderAdmissionLifecycleV1::sameInstrument(
    const InstrumentIdentity& a,
    const InstrumentIdentity& b)
{
    return a.market.canonical_asset == b.market.canonical_asset &&
           a.market.product_class == b.market.product_class &&
           a.market.quote_or_settlement_asset == b.market.quote_or_settlement_asset &&
           a.market.contract_variant == b.market.contract_variant &&
           a.venue_symbol == b.venue_symbol &&
           a.venue_asset_id == b.venue_asset_id;
}

Error MockOrderAdmissionLifecycleV1::admitOrder(const LimitOrderIntent& order)
{
    const CatalogEntry* entry =
        findByCanonicalAssetExact(order.instrument.market.canonical_asset);

    if (entry == nullptr ||
        !entry->enabled ||
        !sameInstrument(order.instrument, entry->instrument()))
        return makeError(
            ErrorClass::UnknownAsset,
            "instrument is not an exact enabled MOCK catalog mapping",
            "UNMAPPED_OR_IDENTITY_MISMATCH");

    const RuleProfile* rules = rulesForExact(*entry);
    if (rules == nullptr || !rules->valid())
        return makeError(
            ErrorClass::VenueUnavailable,
            "MOCK rule profile is absent or invalid",
            "RULE_PROFILE_UNAVAILABLE");

    if (order.time_in_force == TimeInForce::Gtc && !rules->tif_gtc)
        return makeError(
            ErrorClass::UnsupportedCapability,
            "GTC is not supported", "TIF_GTC_UNSUPPORTED");

    if (order.time_in_force == TimeInForce::Ioc && !rules->tif_ioc)
        return makeError(
            ErrorClass::UnsupportedCapability,
            "IOC is not supported", "TIF_IOC_UNSUPPORTED");

    if (order.post_only && !rules->post_only)
        return makeError(
            ErrorClass::UnsupportedCapability,
            "post-only is not supported", "POST_ONLY_UNSUPPORTED");

    if (order.post_only && order.time_in_force == TimeInForce::Ioc)
        return makeError(
            ErrorClass::InvalidRequest,
            "post-only and IOC are incompatible in MOCK v1",
            "POST_ONLY_IOC_CONFLICT");

    if (order.reduce_only && !rules->reduce_only)
        return makeError(
            ErrorClass::UnsupportedCapability,
            "reduce-only is not supported", "REDUCE_ONLY_UNSUPPORTED");

    if (!order.client_order_id.empty() && !rules->client_order_id)
        return makeError(
            ErrorClass::UnsupportedCapability,
            "client_order_id is not supported",
            "CLIENT_ORDER_ID_UNSUPPORTED");

    std::uint64_t price_units = 0;
    if (!divisibleByIncrement(
            order.limit_price, rules->price_increment,
            rules->price_scale, &price_units))
        return makeError(
            ErrorClass::InvalidPriceIncrement,
            "limit price is not exactly on the MOCK price grid",
            "PRICE_INCREMENT");

    std::uint64_t size_units = 0;
    if (!divisibleByIncrement(
            order.quantity, rules->size_increment,
            rules->size_scale, &size_units))
        return makeError(
            ErrorClass::InvalidQuantityIncrement,
            "quantity is not exactly on the MOCK size grid",
            "SIZE_INCREMENT");

    if (!meetsMinimum(size_units, rules->min_size, rules->size_scale))
        return makeError(
            ErrorClass::BelowMinimum,
            "quantity is below MOCK minimum size", "MIN_SIZE");

    if (!meetsMinimumNotional(
            price_units, rules->price_scale,
            size_units, rules->size_scale,
            rules->min_notional))
        return makeError(
            ErrorClass::BelowMinimum,
            "price * quantity is below MOCK minimum notional",
            "MIN_NOTIONAL");

    return noError();
}

bool MockOrderAdmissionLifecycleV1::validTransition(
    OrderLifecycleStatus from,
    OrderLifecycleStatus to)
{
    switch (from) {
    case OrderLifecycleStatus::Accepted:
        return to == OrderLifecycleStatus::Resting ||
               to == OrderLifecycleStatus::PartiallyFilled ||
               to == OrderLifecycleStatus::Filled ||
               to == OrderLifecycleStatus::CancelPending ||
               to == OrderLifecycleStatus::Canceled ||
               to == OrderLifecycleStatus::Rejected ||
               to == OrderLifecycleStatus::VenueTerminated ||
               to == OrderLifecycleStatus::UnknownRequiresReconciliation;
    case OrderLifecycleStatus::Resting:
        return to == OrderLifecycleStatus::PartiallyFilled ||
               to == OrderLifecycleStatus::Filled ||
               to == OrderLifecycleStatus::CancelPending ||
               to == OrderLifecycleStatus::Canceled ||
               to == OrderLifecycleStatus::VenueTerminated ||
               to == OrderLifecycleStatus::UnknownRequiresReconciliation;
    case OrderLifecycleStatus::PartiallyFilled:
        return to == OrderLifecycleStatus::PartiallyFilled ||
               to == OrderLifecycleStatus::Filled ||
               to == OrderLifecycleStatus::CancelPending ||
               to == OrderLifecycleStatus::Canceled ||
               to == OrderLifecycleStatus::VenueTerminated ||
               to == OrderLifecycleStatus::UnknownRequiresReconciliation;
    case OrderLifecycleStatus::CancelPending:
        return to == OrderLifecycleStatus::PartiallyFilled ||
               to == OrderLifecycleStatus::Filled ||
               to == OrderLifecycleStatus::Canceled ||
               to == OrderLifecycleStatus::VenueTerminated ||
               to == OrderLifecycleStatus::UnknownRequiresReconciliation;
    case OrderLifecycleStatus::PendingSubmit:
    case OrderLifecycleStatus::Filled:
    case OrderLifecycleStatus::Canceled:
    case OrderLifecycleStatus::Rejected:
    case OrderLifecycleStatus::VenueTerminated:
    case OrderLifecycleStatus::UnknownRequiresReconciliation:
        return false;
    }
    return false;
}

OrderUpdate MockOrderAdmissionLifecycleV1::makeOrderUpdate(
    const StoredOrder& stored,
    Timestamp timestamp,
    const std::string& causal_request_id,
    const std::string& native_status,
    const Error& terminal_reason)
{
    OrderUpdate u;
    u.venue = context();
    u.instrument = stored.intent.instrument;
    u.causal_request_id = causal_request_id;
    u.local_order_id = stored.intent.local_order_id;
    u.timestamp = timestamp;
    u.status = stored.status;
    u.cumulative_filled_quantity = stored.cumulative_filled_quantity;
    u.remaining_quantity = stored.remaining_quantity;
    u.native_references = stored.native_references;
    u.native_status = native_status;
    u.terminal_reason = terminal_reason;
    return u;
}

} // namespace MockVenueV1
