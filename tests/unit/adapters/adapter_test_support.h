#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "adapters/message_exchange.h"
#include "adapters/simulated_exchange.h"
#include "fake_exchange.h"
#include "message_json.h"
#include "message_subjects.h"

namespace adapter_test {
inline constexpr Timestamp kD1 = 20260105;
inline constexpr Timestamp kD2 = 20260106;
inline constexpr Timestamp kD3 = 20260107;

inline ExecutionOrder order(OrderID id = 40, OrderSide side = OrderSide::Buy,
                            double quantity = 2.0, const Coin& coin = "BTC")
{
    return ExecutionOrder(id, 7, kD1, kD2, coin, side, quantity);
}

inline CoinBarMap bars(double open = 125.0, double high = 140.0,
                        double low = 110.0, double close = 130.0)
{
    BarData value;
    value.open = open;
    value.high = high;
    value.low = low;
    value.close = close;
    value.volume = 10000.0;
    return {{"BTC", value}};
}

inline Fill fill(FillID id = 101, Timestamp at = kD2)
{
    Fill result;
    result.fill_id = id;
    result.order_id = 40;
    result.strategy_id = 7;
    result.timestamp = at;
    result.coin = "BTC";
    result.side = OrderSide::Buy;
    result.quantity = 2.0;
    result.price = 100.0;
    result.commission = 2.0;
    return result;
}

inline ContractMetadata metadata(const std::string& id, Timestamp at = kD2)
{
    return {1, id, "correlation-40", at};
}

template <class T>
std::vector<T> eventsOf(const std::vector<ExchangeEvent>& events)
{
    std::vector<T> selected;
    for (const auto& event : events)
        if (const auto* value = std::get_if<T>(&event)) selected.push_back(*value);
    return selected;
}

// Records attempted logical publications in memory. It deliberately does not
// impersonate broker deduplication, persistence, acknowledgements or subscriptions.
class RecordingBus final : public MessageBus {
public:
    struct Publication {
        std::string subject;
        std::string payload;
        std::string id;
    };
    std::vector<Publication> published;
    std::vector<Publication> attempts;
    bool fail_next = false;
    std::size_t subscription_count = 0;
    std::size_t poll_count = 0;

    void ensureStream(const std::string&, const std::vector<std::string>&) override {}
    void publish(const std::string& subject, const std::string& payload,
                 const std::string& id) override
    {
        attempts.push_back({subject, payload, id});
        if (fail_next) {
            fail_next = false;
            throw std::runtime_error("Injected publication failure");
        }
        published.push_back({subject, payload, id});
    }
    SubscriptionID subscribe(const DurableConsumerOptions&, Handler) override
    {
        return ++subscription_count;
    }
    std::size_t poll(SubscriptionID, int, std::int64_t) override { ++poll_count; return 0; }
    void close(SubscriptionID) override {}
    void flush() override {}
};
} // namespace adapter_test
