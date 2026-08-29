#include "equal_weight_sizer.h"
#include "fake_exchange.h"
#include "indicator_engine.h"
#include "recovery_coordinator.h"
#include "risk_constraints.h"
#include "sqlite_state_store.h"
#include "strategy.h"
#include "strategy_instance.h"
#include "threshold_rebalance_policy.h"
#include "trading_engine.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>


namespace {

constexpr double INITIAL_CASH = 100000.0;
constexpr double PRICE = 100.0;
constexpr StrategyID STRATEGY_ID = 1;
const Coin TEST_COIN = "BTC";


/**************************************************************************************
 * Type    : ScheduledSignalStrategy
 * Purpose : Tiny deterministic strategy used only by the fake-live integration test
 *
 * 100..299 -> long
 * 300..499 -> flat
 * 500..599 -> long
 * >= 600   -> flat
 **************************************************************************************/
class ScheduledSignalStrategy final : public Strategy {
public:
    ScheduledSignalStrategy()
        : Strategy(
            "FakeLiveStrategy",
            1,
            nullptr,
            nullptr,
            std::numeric_limits<unsigned int>::max()
        )
    {}

    void updateSignals(
        const MarketData& marketData,
        Timestamp ts,
        SignalState& signalState,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)marketData;
        (void)indicators;

        if ((ts >= 100 && ts < 300) || (ts >= 500 && ts < 600))
            signalState.set(TEST_COIN, 1.0);
        else
            signalState.set(TEST_COIN, 0.0);
    }

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        return {};
    }
};


StrategyPortfolio makeStrategies()
{
    StrategyPortfolio strategies;
    strategies.emplace_back(
        STRATEGY_ID,
        std::make_unique<ScheduledSignalStrategy>(),
        1.0,
        std::make_unique<EqualWeightSizer>(0.10),
        RiskConstraints(1.0, 1.0),
        // Zero threshold intentionally keeps the test target explicit after partial fills.
        std::make_unique<ThresholdRebalancePolicy>(0.0)
    );
    return strategies;
}


MarketData makeMarketData()
{
    MarketData data;
    const std::vector<Timestamp> timestamps{
        100, 200, 300, 310, 400, 500, 550, 560, 600
    };

    unsigned int barNumber = 1;
    for (const Timestamp ts : timestamps) {
        BarData bar;
        bar.open = PRICE;
        bar.high = PRICE;
        bar.low = PRICE;
        bar.close = PRICE;
        bar.volume = 1.0;
        bar.barNumber = barNumber++;
        data[ts][TEST_COIN] = bar;
    }

    return data;
}


PriceSnapshot marks(double price = PRICE)
{
    PriceSnapshot result;
    result.set(TEST_COIN, price);
    return result;
}


ExecutionReferencePrices executionPrices(double price = PRICE)
{
    ExecutionReferencePrices result;
    result.set(TEST_COIN, price);
    return result;
}


void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("STEP 26 TEST FAILED: " + message);
}


void requireNear(double actual, double expected, const std::string& message, double tolerance = 1e-9)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(
            "STEP 26 TEST FAILED: " + message +
            " actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected)
        );
}


void printPass(const std::string& text)
{
    std::cout << "[PASS] " << text << '\n';
}


void removeStateFiles(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

}


int main()
{
    try {
        const std::filesystem::path statePath =
            std::filesystem::temp_directory_path() / "algotrading_step26_fake_live.sqlite";
        removeStateFiles(statePath);

        const MarketData marketData = makeMarketData();
        IndicatorEngine indicators;
        FakeExchange exchange(INITIAL_CASH, 0.0);

        std::cout << "============================================================\n";
        std::cout << "STEP 26 - FAKE LIVE END-TO-END TEST\n";
        std::cout << "============================================================\n";

        /**************************************************************************
         * Session 1: submit, ACK, partial fill, duplicate-order protection
         **************************************************************************/
        {
            StrategyPortfolio strategies = makeStrategies();
            Account account(INITIAL_CASH);
            TradeRecorder trades;
            SQLiteStateStore store(statePath.string());
            TradingEngine engine(strategies, account, trades, indicators, exchange);
            engine.attachStateStore(store);

            engine.onBarClose(marketData, 100, marks());
            engine.executePendingPlans(101, executionPrices());

            require(exchange.submitCount() == 1, "initial target must submit exactly one order");
            require(engine.orderManager().find(1) != nullptr, "order 1 must be tracked locally");
            require(
                engine.orderManager().find(1)->status == ExecutionOrderStatus::Submitted,
                "order 1 must remain Submitted until fake ACK"
            );
            printPass("order submission is asynchronous");

            exchange.acceptOrder(1, 101);
            engine.processExchangeEvents();
            require(
                engine.orderManager().find(1)->status == ExecutionOrderStatus::Accepted,
                "order 1 ACK must reach OrderManager"
            );
            printPass("delayed ACK processed");

            const double requested = engine.orderManager().find(1)->request.quantity;
            const double firstFillQuantity = requested * 0.30;
            exchange.fillOrder(1, firstFillQuantity, PRICE, 102);
            engine.processExchangeEvents();

            requireNear(account.positions().get(TEST_COIN), firstFillQuantity, "partial fill account position");
            requireNear(
                engine.orderManager().pendingSignedQuantity(STRATEGY_ID, TEST_COIN),
                requested - firstFillQuantity,
                "remaining pending quantity after partial fill"
            );
            printPass("30% partial fill updates Account and pending quantity");

            // Same target on a later bar must see filled + pending as the effective position.
            engine.onBarClose(marketData, 200, marks());
            engine.executePendingPlans(201, executionPrices());
            require(exchange.submitCount() == 1, "pending remainder must prevent duplicate submission");
            printPass("pending-order-aware execution prevents duplicate BUY");
        }

        /**************************************************************************
         * Session 2: restart, clean reconciliation, cancel-before-replace, fills
         **************************************************************************/
        {
            StrategyPortfolio strategies = makeStrategies();
            Account account(INITIAL_CASH);
            TradeRecorder trades;
            SQLiteStateStore store(statePath.string());
            TradingEngine engine(strategies, account, trades, indicators, exchange);
            RecoveryCoordinator recovery;

            const RecoveryResult recovered = recovery.recover(
                engine,
                store,
                exchange.snapshot(210)
            );

            require(recovered.restored_persisted_state, "restart must restore persisted state");
            require(recovered.ready_for_trading, "matching exchange snapshot must reconcile cleanly");
            require(engine.tradingEnabled(), "clean reconciliation must resume trading");
            requireNear(account.positions().get(TEST_COIN), exchange.position(TEST_COIN), "restored account position");
            require(engine.orderManager().find(1) != nullptr, "partial order must survive restart");
            requireNear(
                engine.orderManager().find(1)->filled_quantity,
                exchange.snapshot(210).open_orders.front().filled_quantity,
                "restored filled order quantity"
            );
            printPass("SQLite restart + exchange reconciliation restores partial order");

            // Strategy becomes flat while BUY remainder is still open: cancel first, never cross it.
            engine.onBarClose(marketData, 300, marks());
            engine.executePendingPlans(301, executionPrices());
            require(exchange.cancelRequested(1), "changed target must request cancel of old BUY");
            require(exchange.cancelRequestCount(1) == 1, "cancel must be sent exactly once initially");
            require(exchange.submitCount() == 1, "no opposing SELL may be submitted before cancel confirmation");

            // Another cycle while cancel is pending must not spam cancelOrder().
            engine.onBarClose(marketData, 310, marks());
            engine.executePendingPlans(311, executionPrices());
            require(exchange.cancelRequestCount(1) == 1, "pending cancel must not be submitted repeatedly");
            require(exchange.submitCount() == 1, "cancel-pending state must not submit replacement order");
            printPass("cancel-before-replace and cancel de-duplication work");

            exchange.confirmCancel(1, 320);
            engine.processExchangeEvents();
            require(
                engine.orderManager().find(1)->status == ExecutionOrderStatus::Canceled,
                "cancel confirmation must close old order lifecycle"
            );

            // Next completed decision can now submit the flattening SELL for the filled 30%.
            engine.onBarClose(marketData, 400, marks());
            engine.executePendingPlans(401, executionPrices());
            require(exchange.submitCount() == 2, "flat target after confirmed cancel must submit SELL");
            require(engine.orderManager().find(2) != nullptr, "replacement SELL must be order 2");
            require(engine.orderManager().find(2)->request.side == OrderSide::Sell, "order 2 must be SELL");

            exchange.acceptOrder(2, 401);
            engine.processExchangeEvents();
            const double sellQuantity = engine.orderManager().find(2)->request.quantity;
            exchange.fillOrder(2, sellQuantity, 101.0, 402);
            engine.processExchangeEvents();

            requireNear(account.positions().get(TEST_COIN), 0.0, "flattening fill must close local position");
            requireNear(exchange.position(TEST_COIN), 0.0, "flattening fill must close exchange position");
            require(!trades.closedTrades().empty(), "round trip must create a closed TradeRecord");
            printPass("confirmed cancel -> replacement SELL -> flat account");

            // Replayed WebSocket/exchange fill must be idempotent.
            const double cashBeforeReplay = account.cash();
            exchange.replayLastFill();
            engine.processExchangeEvents();
            requireNear(account.cash(), cashBeforeReplay, "duplicate FillID must not change cash");
            requireNear(account.positions().get(TEST_COIN), 0.0, "duplicate FillID must not change position");
            printPass("duplicate FillID replay is idempotent");

            // New entry gets rejected; a later decision is allowed to create a fresh order.
            engine.onBarClose(marketData, 500, marks());
            engine.executePendingPlans(501, executionPrices());
            require(exchange.submitCount() == 3, "second entry signal must submit order 3");
            exchange.rejectOrder(3, 502, "intentional Step 26 reject");
            engine.processExchangeEvents();
            require(
                engine.orderManager().find(3)->status == ExecutionOrderStatus::Rejected,
                "exchange rejection must be terminal locally"
            );
            printPass("rejected order becomes terminal without changing Account");

            engine.onBarClose(marketData, 550, marks());
            engine.executePendingPlans(551, executionPrices());
            require(exchange.submitCount() == 4, "later decision must retry after rejected order");
            exchange.acceptOrder(4, 551);
            engine.processExchangeEvents();

            // Leave the accepted order deliberately unfilled for another decision cycle.
            engine.onBarClose(marketData, 560, marks());
            engine.executePendingPlans(561, executionPrices());
            require(exchange.submitCount() == 4, "delayed accepted order must prevent duplicate retry");
            printPass("delayed fill remains pending without duplicate order");

            const double retryQuantity = engine.orderManager().find(4)->request.quantity;
            exchange.fillOrder(4, retryQuantity * 0.40, PRICE, 562);
            engine.processExchangeEvents();
            exchange.fillOrder(4, retryQuantity * 0.60, PRICE, 563);
            engine.processExchangeEvents();
            require(
                engine.orderManager().find(4)->status == ExecutionOrderStatus::Filled,
                "partial + final fills must finish order 4"
            );
            requireNear(account.positions().get(TEST_COIN), retryQuantity, "partial + final fills must sum correctly");
            printPass("partial + final fills complete order correctly");
        }

        /**************************************************************************
         * Session 3: deliberately corrupt external snapshot; startup must stay paused
         **************************************************************************/
        {
            StrategyPortfolio strategies = makeStrategies();
            Account account(INITIAL_CASH);
            TradeRecorder trades;
            SQLiteStateStore store(statePath.string());
            TradingEngine engine(strategies, account, trades, indicators, exchange);
            RecoveryCoordinator recovery;

            ExchangeSnapshot badSnapshot = exchange.snapshot(700);
            badSnapshot.positions[TEST_COIN] += 1.0; // deliberate external mismatch

            const RecoveryResult recovered = recovery.recover(engine, store, badSnapshot);
            require(!recovered.ready_for_trading, "position mismatch must fail reconciliation");
            require(!engine.tradingEnabled(), "failed reconciliation must leave engine paused");
            require(!recovered.reconciliation.clean(), "mismatch report must contain an issue");
            printPass("reconciliation mismatch blocks trading");
        }

        removeStateFiles(statePath);

        std::cout << "\n============================================================\n";
        std::cout << "STEP 26 RESULT: ALL FAKE-LIVE TESTS PASSED\n";
        std::cout << "============================================================\n";
        std::cout << "Next: run the validated PureRSI backtests as regression tests.\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "\n" << ex.what() << '\n';
        return 1;
    }
}
