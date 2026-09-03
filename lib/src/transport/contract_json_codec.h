#pragma once

#include <string>

#include "account_snapshot.h"
#include "decision_batch.h"
#include "execution_commands.h"
#include "execution_cycle_complete.h"
#include "execution_events.h"
#include "execution_price_snapshot.h"
#include "exchange_snapshot_event.h"
#include "exchange_snapshot_request.h"
#include "market_data_release.h"
#include "market_slice_closed.h"
#include "market_slice_snapshot.h"
#include "order_planning.h"
#include "strategy_intent_batch.h"


/**************************************************************************************
 * Type    : ContractJsonCodec
 * Purpose : Stable JSON wire representation for service-boundary DTOs
 **************************************************************************************/
namespace ContractJsonCodec {

std::string encode(const MarketDataReleaseRequest& value);
MarketDataReleaseRequest decodeMarketDataReleaseRequest(const std::string& payload);

std::string encode(const MarketSliceClosed& value);
MarketSliceClosed decodeMarketSliceClosed(const std::string& payload);

std::string encode(const MarketSliceSnapshot& value);
MarketSliceSnapshot decodeMarketSliceSnapshot(const std::string& payload);

std::string encode(const StrategyIntentBatch& value);
StrategyIntentBatch decodeStrategyIntentBatch(const std::string& payload);

std::string encode(const DecisionBatch& value);
DecisionBatch decodeDecisionBatch(const std::string& payload);

std::string encode(const ExecutionPriceSnapshot& value);
ExecutionPriceSnapshot decodeExecutionPriceSnapshot(const std::string& payload);

std::string encode(const SubmitOrderCommand& value);
SubmitOrderCommand decodeSubmitOrderCommand(const std::string& payload);

std::string encode(const CancelOrderCommand& value);
CancelOrderCommand decodeCancelOrderCommand(const std::string& payload);

std::string encode(const OrderUpdateEvent& value);
OrderUpdateEvent decodeOrderUpdateEvent(const std::string& payload);

std::string encode(const FillEvent& value);
FillEvent decodeFillEvent(const std::string& payload);

std::string encode(const AccountSnapshot& value);
AccountSnapshot decodeAccountSnapshot(const std::string& payload);

std::string encode(const ExecutionCycleComplete& value);
ExecutionCycleComplete decodeExecutionCycleComplete(const std::string& payload);

std::string encode(const ExchangeSnapshotEvent& value);
ExchangeSnapshotEvent decodeExchangeSnapshotEvent(const std::string& payload);

std::string encode(const ExchangeSnapshotRequest& value);
ExchangeSnapshotRequest decodeExchangeSnapshotRequest(const std::string& payload);

std::string encode(const OrderPlanningRequest& value);
OrderPlanningRequest decodeOrderPlanningRequest(const std::string& payload);

std::string encode(const OrderPlanBatch& value);
OrderPlanBatch decodeOrderPlanBatch(const std::string& payload);

}
