// Real-adapter fixture. The Python runner owns isolated infrastructure and restarts.
#include "jetstream_bus.h"
#include "postgres_state_store.h"

#include <libpq-fe.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const char* setting(const char* name)
{
    const char* value = std::getenv(name);
    require(value && *value, "Missing isolated test connection setting");
    return value;
}

void sql(const char* command)
{
    PGconn* connection = PQconnectdb(setting("TEST_POSTGRES_DSN"));
    PGresult* result = PQexec(connection, command);
    const bool success = result && PQresultStatus(result) == PGRES_COMMAND_OK;
    PQclear(result);
    PQfinish(connection);
    require(success, "Failed to configure isolated transaction-failure fixture");
}

Fill testFill()
{
    Fill fill;
    fill.fill_id = 7;
    fill.order_id = 1;
    fill.strategy_id = 1;
    fill.timestamp = 20200102;
    fill.coin = "BTC";
    fill.quantity = 0.125;
    fill.price = 250.0;
    return fill;
}

DurableConsumerOptions consumerOptions()
{
    DurableConsumerOptions options;
    options.stream = "VALIDATION_RECOVERY";
    options.durable_name = "validation-recovery";
    options.subject = "validation.>";
    options.ack_wait_ms = 200;
    return options;
}

void checkRecovered(const PostgresStateStore& store)
{
    const auto state = store.load();
    require(state.has_value(), "Committed snapshot missing after restart");
    require(state->last_execution_timestamp == 20200102, "Execution timestamp changed");
    require(state->last_bar_close_timestamp == 20200101, "Decision timestamp changed");
    require(state->next_order_id == 2, "Next order identity changed after restart");
    require(state->account_cash == 968.75, "Cash changed or duplicate fill applied");
    require(state->account_positions.at("BTC") == 0.125, "Position sign/quantity changed");
    require(state->processed_fill_ids == std::vector<FillID>{7}, "Processed fill identity changed");
    const auto fills = store.loadFills();
    require(fills.size() == 1, "Fill audit has missing or duplicate rows");
    require(fills[0].fill_id == 7 && fills[0].timestamp == 20200102 &&
            fills[0].quantity == 0.125 && fills[0].price == 250.0,
            "Persisted fill economics changed");
}

void receiveOne(JetStreamBus& bus, MessageBus::SubscriptionID subscription)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (bus.poll(subscription, 1, 250) == 1)
            return;
    }
    throw std::runtime_error("Expected durable delivery did not arrive");
}

void seedAndCrash()
{
    PostgresStateStore store(setting("TEST_POSTGRES_DSN"));
    require(!store.load() && store.loadFills().empty(), "Fixture database is not empty");
    TradingStateSnapshot initial;
    initial.account_cash = 1000.0;
    initial.last_bar_close_timestamp = 20200101;
    store.save(initial);

    // Fail the snapshot write after the fill INSERT, exercising actual SQL rollback.
    sql("ALTER TABLE trading_runtime_state ADD CONSTRAINT validation_failure CHECK (schema_version <> 999);");
    auto invalid = initial;
    invalid.schema_version = 999;
    invalid.account_cash = 0.0;
    bool rejected = false;
    try {
        store.save(invalid, testFill());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected && store.loadFills().empty() && store.load()->account_cash == 1000.0,
            "Failed transaction did not roll back both snapshot and fill");
    sql("ALTER TABLE trading_runtime_state DROP CONSTRAINT validation_failure;");

    JetStreamBus bus(setting("TEST_NATS_URL"));
    bus.ensureStream("VALIDATION_RECOVERY", {"validation.>"});
    bus.publish("validation.fill", "fill:7", "fill:7");
    bus.publish("validation.fill", "fill:7", "fill:7");
    const auto subscription = bus.subscribe(consumerOptions(), [&](const BusMessage& message) {
        require(message.payload == "fill:7", "Unexpected fixture payload");
        auto state = initial;
        const auto fill = testFill();
        state.account_cash -= fill.quantity * fill.price;
        state.account_positions[fill.coin] = fill.signedQuantity();
        state.last_execution_timestamp = fill.timestamp;
        state.next_order_id = 2;
        state.processed_fill_ids.push_back(fill.fill_id);
        store.save(state, fill);
        checkRecovered(store);
        // Deliberately bypass destructors and ACK after the successful DB commit.
        std::_Exit(73);
        return DurableMessageDisposition::Ack;
    });
    receiveOne(bus, subscription);
    throw std::runtime_error("Crash injection did not execute");
}

void recoverAndAcknowledge()
{
    PostgresStateStore store(setting("TEST_POSTGRES_DSN"));
    checkRecovered(store);
    JetStreamBus bus(setting("TEST_NATS_URL"));
    auto incompatible = consumerOptions();
    incompatible.subject = "validation.other";
    bool rejected = false;
    try {
        bus.subscribe(incompatible, [](const BusMessage&) { return DurableMessageDisposition::Ack; });
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Restart accepted an incompatible durable filter");
    const auto subscription = bus.subscribe(consumerOptions(), [&](const BusMessage& message) {
        require(message.payload == "fill:7", "Restart delivered an unexpected payload");
        const auto state = store.load();
        require(std::find(state->processed_fill_ids.begin(), state->processed_fill_ids.end(), 7)
                    != state->processed_fill_ids.end(), "Committed fill deduplication identity missing");
        // The caller owns economic deduplication. The store additionally deduplicates audit IDs.
        store.save(*state, testFill());
        return DurableMessageDisposition::Ack;
    });
    receiveOne(bus, subscription);
    bus.flush();
    checkRecovered(store);
    require(bus.poll(subscription, 1, 300) == 0, "Duplicate publish or ACKed message was delivered");
    bus.close(subscription);
}

void verifyAcknowledgedRestart()
{
    PostgresStateStore store(setting("TEST_POSTGRES_DSN"));
    checkRecovered(store);
    JetStreamBus bus(setting("TEST_NATS_URL"));
    int deliveries = 0;
    const auto subscription = bus.subscribe(consumerOptions(), [&](const BusMessage&) {
        ++deliveries;
        return DurableMessageDisposition::Ack;
    });
    require(bus.poll(subscription, 1, 300) == 0 && deliveries == 0,
            "Acknowledged message reappeared after broker restart");
    bus.close(subscription);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Expected seed, recover or verify phase");
        const std::string phase = argv[1];
        if (phase == "seed") seedAndCrash();
        else if (phase == "recover") recoverAndAcknowledge();
        else if (phase == "verify") verifyAcknowledgedRestart();
        else throw std::runtime_error("Unknown test phase");
        std::cout << "TRANSPORT-PERSISTENCE: PASS: " << phase << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TRANSPORT-PERSISTENCE: FAIL: " << error.what() << '\n';
        return 1;
    }
}
