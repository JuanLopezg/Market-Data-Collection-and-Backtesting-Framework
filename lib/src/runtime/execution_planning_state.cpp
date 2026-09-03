#include "execution_planning_state.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

void hashByte(std::uint64_t& hash, std::uint8_t value)
{
    hash ^= value;
    hash *= FNV_PRIME;
}

template <typename UInt>
void hashUnsigned(std::uint64_t& hash, UInt value)
{
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        hashByte(hash, static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
}

void hashString(std::uint64_t& hash, const std::string& value)
{
    hashUnsigned(hash, static_cast<std::uint64_t>(value.size()));
    for (const unsigned char c : value)
        hashByte(hash, c);
}

void hashDouble(std::uint64_t& hash, double value)
{
    hashUnsigned(hash, std::bit_cast<std::uint64_t>(value));
}

} // namespace


std::uint64_t executionPlanningStateRevision(
    const ExecutionPlanningStateSnapshot& state
)
{
    std::uint64_t hash = FNV_OFFSET;

    std::vector<StrategyID> strategyIds = state.strategy_ids;
    std::sort(strategyIds.begin(), strategyIds.end());
    hashUnsigned(hash, static_cast<std::uint64_t>(strategyIds.size()));

    for (const StrategyID strategyId : strategyIds) {
        hashUnsigned(hash, static_cast<std::uint64_t>(strategyId));

        const auto positionsIt = state.strategy_positions.find(strategyId);
        std::vector<std::pair<Coin, double>> positions;
        if (positionsIt != state.strategy_positions.end()) {
            positions.assign(positionsIt->second.begin(), positionsIt->second.end());
            std::sort(positions.begin(), positions.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
        }

        hashUnsigned(hash, static_cast<std::uint64_t>(positions.size()));
        for (const auto& [coin, quantity] : positions) {
            hashString(hash, coin);
            hashDouble(hash, quantity);
        }
    }

    std::vector<const TrackedOrder*> orders;
    orders.reserve(state.orders.size());
    for (const TrackedOrder& order : state.orders)
        orders.push_back(&order);

    std::sort(orders.begin(), orders.end(), [](const TrackedOrder* a, const TrackedOrder* b) {
        return a->request.order_id < b->request.order_id;
    });

    hashUnsigned(hash, static_cast<std::uint64_t>(orders.size()));
    for (const TrackedOrder* order : orders) {
        hashUnsigned(hash, static_cast<std::uint64_t>(order->request.order_id));
        hashUnsigned(hash, static_cast<std::uint64_t>(order->request.strategy_id));
        hashString(hash, order->request.coin);
        hashUnsigned(hash, static_cast<std::uint64_t>(order->request.side));
        hashDouble(hash, order->request.quantity);
        hashUnsigned(hash, static_cast<std::uint64_t>(order->status));
        hashDouble(hash, order->filled_quantity);
        hashUnsigned(hash, static_cast<std::uint64_t>(order->cancel_requested ? 1U : 0U));
    }

    hashUnsigned(hash, static_cast<std::uint64_t>(state.next_order_id));

    // Reserve zero as "missing/not initialized" in the wire contract.
    return hash == 0 ? 1 : hash;
}


ExecutionPlanningStateSnapshot makeExecutionPlanningStateSnapshot(
    const std::vector<StrategyID>& strategyIds,
    const StrategyPositionSnapshot& strategyPositions,
    const OrderManager& orderManager,
    OrderID nextOrderId
)
{
    ExecutionPlanningStateSnapshot result;
    result.strategy_ids = strategyIds;
    result.next_order_id = nextOrderId;

    for (const auto& [strategyId, positions] : strategyPositions)
        result.strategy_positions.emplace(strategyId, positions.values());

    result.orders.reserve(orderManager.orders().size());
    for (const auto& [orderId, order] : orderManager.orders()) {
        (void)orderId;
        result.orders.push_back(order);
    }

    result.state_revision = executionPlanningStateRevision(result);
    return result;
}
