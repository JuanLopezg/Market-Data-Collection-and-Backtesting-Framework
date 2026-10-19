// The PAPER trading chain end to end; rules C1..C9 in tests/specs/chain.md.

#include <gtest/gtest.h>

#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

#include "chain_harness.h"

namespace {

using chain_test::CsvBar;
using chain_test::PaperChain;
using chain_test::Rows;

constexpr int TOTAL_DAYS = 45;
constexpr int BUSINESS_START_DAY = 31; // days 1..30 are warm-up history already visible at startup
constexpr int JUMP_DAY = 32;  // J: RUNUP and SWING jump +30, RSI crosses above 80
constexpr int DROP_DAY = 40;  // D: SWING falls 30, RSI drops below 70
constexpr double INITIAL_CASH = 100000.0;
constexpr double COMMISSION_RATE = 0.001;

// Day 1 is 2025-01-01.
std::tm dayToTm(int day)
{
    std::tm date{};
    date.tm_year = 2025 - 1900;
    date.tm_mon = 0;
    date.tm_mday = day;
    timegm(&date); // normalizes day overflow into the right month
    return date;
}

std::string isoDate(int day)
{
    const std::tm date = dayToTm(day);
    std::ostringstream text;
    text << std::put_time(&date, "%Y-%m-%d");
    return text.str();
}

long dateKey(int day)
{
    const std::tm date = dayToTm(day);
    return (date.tm_year + 1900) * 10000L + (date.tm_mon + 1) * 100L + date.tm_mday;
}

// Daily close changes from the table in tests/specs/chain.md.
double closeChange(const std::string& symbol, int day)
{
    const double alternating = (day % 2 == 0) ? 1.0 : -1.0;
    if (symbol == "RUNUP")
        return day < JUMP_DAY ? alternating : (day == JUMP_DAY ? 30.0 : 1.0);
    if (symbol == "SWING")
        return day < JUMP_DAY ? alternating : (day == JUMP_DAY ? 30.0 : (day == DROP_DAY ? -30.0 : 1.0));
    // SLIDE mirrors the alternation and only falls afterwards.
    return day < JUMP_DAY ? -alternating : (day == JUMP_DAY ? -30.0 : -1.0);
}

struct Market {
    std::vector<CsvBar> bars;
    std::map<std::string, std::map<int, CsvBar>> bySymbol;

    const CsvBar& bar(const std::string& symbol, int day) const { return bySymbol.at(symbol).at(day); }
};

Market buildMarket()
{
    Market market;
    const std::vector<std::string> symbols{"RUNUP", "SLIDE", "SWING"};
    std::map<std::string, double> previousClose{{"RUNUP", 100.0}, {"SLIDE", 100.0}, {"SWING", 100.0}};
    for (int day = 1; day <= TOTAL_DAYS; ++day) {
        for (const auto& symbol : symbols) {
            const double open = previousClose[symbol] + 0.5;
            const double close = day == 1 ? 100.0 : previousClose[symbol] + closeChange(symbol, day);
            CsvBar bar{isoDate(day), symbol, open, std::max(open, close) + 1.0, std::min(open, close) - 1.0,
                       close, 1000000.0};
            market.bars.push_back(bar);
            market.bySymbol[symbol][day] = bar;
            previousClose[symbol] = close;
        }
    }
    return market;
}

struct Fill {
    long fill_id;
    std::string coin;
    int side; // 0 buy, 1 sell (contracts Side)
    double quantity;
    double price;
    double commission;
};

std::vector<Fill> readFills(const PaperChain& chain)
{
    std::vector<Fill> fills;
    for (const auto& row : chain.query(
             "SELECT fill_id, coin, side, quantity, price, commission FROM trading_fills ORDER BY fill_id"))
        fills.push_back({std::stol(row[0]), row[1], std::stoi(row[2]), std::stod(row[3]), std::stod(row[4]),
                         std::stod(row[5])});
    return fills;
}

double totalQuantity(const std::vector<Fill>& fills, const std::string& coin, int side)
{
    double quantity = 0.0;
    for (const auto& fill : fills)
        if (fill.coin == coin && fill.side == side)
            quantity += fill.quantity;
    return quantity;
}

void expectNear(double actual, double expected, double relative, const std::string& what)
{
    EXPECT_NEAR(actual, expected, std::abs(expected) * relative + 1e-9) << what;
}

std::unique_ptr<PaperChain> startChain(const Market& market)
{
    chain_test::ChainSettings settings;
    settings.bars = market.bars;
    settings.first_date = isoDate(BUSINESS_START_DAY);
    settings.seconds_per_business_day = 15.0; // the chain needs about 6 s per day
    settings.initial_cash = INITIAL_CASH;
    settings.commission_rate = COMMISSION_RATE;
    settings.market_warmup_days = 30;
    return std::make_unique<PaperChain>(settings);
}

// C1..C8 on a finished chain run.
void expectTradingOutcome(const PaperChain& chain, const Market& market)
{
    const std::vector<Fill> fills = readFills(chain);
    ASSERT_FALSE(fills.empty()) << chain.logs();

    // C3: SLIDE never traded.
    EXPECT_EQ(totalQuantity(fills, "SLIDE", 0) + totalQuantity(fills, "SLIDE", 1), 0.0);

    // C1: every buy is RUNUP or SWING at open(J+1); C2: SWING sells at open(D+1).
    for (const auto& fill : fills) {
        if (fill.side == 0) {
            ASSERT_TRUE(fill.coin == "RUNUP" || fill.coin == "SWING") << "unexpected buy of " << fill.coin;
            expectNear(fill.price, market.bar(fill.coin, JUMP_DAY + 1).open, 1e-9,
                       "C1 buy price of " + fill.coin + " must be open(J+1)");
        }
        else {
            ASSERT_EQ(fill.coin, "SWING") << "C2 only SWING is sold";
            expectNear(fill.price, market.bar("SWING", DROP_DAY + 1).open, 1e-9,
                       "C2 sell price of SWING must be open(D+1)");
        }
        // C5: commission is the configured rate of the fill value.
        expectNear(fill.commission, fill.quantity * fill.price * COMMISSION_RATE, 1e-6,
                   "C5 commission of fill " + std::to_string(fill.fill_id));
    }

    const double runupQuantity = totalQuantity(fills, "RUNUP", 0);
    const double swingBought = totalQuantity(fills, "SWING", 0);
    ASSERT_GT(runupQuantity, 0.0) << "C1 RUNUP bought";
    ASSERT_GT(swingBought, 0.0) << "C1 SWING bought";
    EXPECT_EQ(totalQuantity(fills, "RUNUP", 1), 0.0) << "C2 RUNUP still held";
    // C2: the exit flattens exactly what was bought.
    EXPECT_DOUBLE_EQ(totalQuantity(fills, "SWING", 1), swingBought) << "C2 SWING flat";

    // C4: 10% of the 100,000 USD equity per entry (close(T) sizing vs open(T+1) fill: within 1%).
    expectNear(runupQuantity * market.bar("RUNUP", JUMP_DAY + 1).open, 0.10 * INITIAL_CASH, 0.01,
               "C4 RUNUP entry notional");
    expectNear(swingBought * market.bar("SWING", JUMP_DAY + 1).open, 0.10 * INITIAL_CASH, 0.01,
               "C4 SWING entry notional");

    // C5: cash from fills alone.
    double expectedCash = INITIAL_CASH;
    for (const auto& fill : fills)
        expectedCash += (fill.side == 0 ? -1.0 : 1.0) * fill.quantity * fill.price - fill.commission;

    // C7: one equity row per day and the last one values cash + RUNUP at close(last day).
    const Rows equity = chain.query("SELECT date, cash, positions_value, equity FROM trading_equity_daily ORDER BY date");
    ASSERT_FALSE(equity.empty());
    std::map<long, int> rowsPerDay;
    for (const auto& row : equity)
        ++rowsPerDay[std::stol(row[0])];
    for (const auto& [date, count] : rowsPerDay)
        EXPECT_EQ(count, 1) << "C7 equity rows for " << date;
    const auto& last = equity.back();
    EXPECT_EQ(std::stol(last[0]), dateKey(TOTAL_DAYS)) << "C7 last equity day";
    expectNear(std::stod(last[1]), expectedCash, 1e-9, "C5 final cash");
    expectNear(std::stod(last[2]), runupQuantity * market.bar("RUNUP", TOTAL_DAYS).close, 1e-9,
               "C7 positions value at close(last day)");
    expectNear(std::stod(last[3]), expectedCash + runupQuantity * market.bar("RUNUP", TOTAL_DAYS).close, 1e-9,
               "C7 equity = cash + positions");

    // C6: the closed SWING position's gross PnL.
    const Rows closed = chain.query(
        "SELECT coin, entry_price, exit_price, exit_quantity, gross_pnl FROM trading_closed_positions");
    ASSERT_EQ(closed.size(), 1u) << "C2 exactly one closed position";
    EXPECT_EQ(closed[0][0], "SWING");
    const double expectedPnl = (market.bar("SWING", DROP_DAY + 1).open - market.bar("SWING", JUMP_DAY + 1).open) *
                               swingBought;
    expectNear(std::stod(closed[0][4]), expectedPnl, 1e-9, "C6 SWING gross PnL");

    // Open positions: only RUNUP, long, with the bought quantity.
    const Rows open = chain.query("SELECT coin, side, quantity FROM trading_open_positions");
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0][0], "RUNUP");
    expectNear(std::stod(open[0][2]), runupQuantity, 1e-12, "open RUNUP quantity");

    // C8: reconciliation clean on every recorded day.
    for (const auto& row : chain.query("SELECT date, state FROM trading_reconciliation_daily"))
        EXPECT_EQ(row[1], "CLEAN") << "C8 reconciliation on " << row[0];
}

class PaperChainTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!test_support::dockerAvailable())
            GTEST_SKIP() << "Docker is not available";
    }
};

TEST_F(PaperChainTest, C1toC8_PureRsiEntriesExitsAndAccountingOverFortyFiveDays)
{
    const Market market = buildMarket();
    auto chain = startChain(market);
    ASSERT_TRUE(chain->waitForEquityDay(dateKey(TOTAL_DAYS), std::chrono::seconds(600))) << chain->logs();
    expectTradingOutcome(*chain, market);
}

TEST_F(PaperChainTest, C9_CrashedServicesRecoverWithoutDuplicateOrMissingFills)
{
    const Market market = buildMarket();
    auto chain = startChain(market);

    // Crash between the entry fills (J+1) and the exit decision (D).
    ASSERT_TRUE(chain->waitForEquityDay(dateKey(JUMP_DAY + 2), std::chrono::seconds(600))) << chain->logs();
    chain->crashAndRestart("execution");
    chain->crashAndRestart("strategy");
    chain->crashAndRestart("sim-exchange");

    ASSERT_TRUE(chain->waitForEquityDay(dateKey(TOTAL_DAYS), std::chrono::seconds(600))) << chain->logs();
    expectTradingOutcome(*chain, market);
}

} // namespace

namespace {

// C10: a late account snapshot (execution down for longer than the services' redelivery
// window) must delay trading, not stop it. The day's decision happens once execution is
// back and the chain finishes every day; holdings at the end are the same as without
// the outage (RUNUP held, SWING sold, SLIDE never traded).
// FINDING: portfolio risk answers "account snapshot not here yet" with Retry; every
// consumer has max_deliver = 20 (about 20 s), so JetStream drops the day's strategy
// intents and every later day waits on that decision forever (tests/specs/chain.md).
TEST_F(PaperChainTest, DISABLED_C10_LateAccountSnapshotDelaysButDoesNotStopTrading)
{
    const Market market = buildMarket();
    auto chain = startChain(market);

    // Take execution down just before the strategy publishes the intents of day J,
    // for 40 s of real time (two to three business days at 15 s/day).
    ASSERT_TRUE(test_support::waitUntil([&] { return chain->newestStrategyDay() >= dateKey(JUMP_DAY - 1); },
                                        std::chrono::seconds(600), std::chrono::milliseconds(200)))
        << chain->logs();
    chain->crashAndRestart("execution", std::chrono::seconds(40));

    ASSERT_TRUE(chain->waitForEquityDay(dateKey(TOTAL_DAYS), std::chrono::seconds(600))) << chain->logs();
    const std::vector<Fill> fills = readFills(*chain);
    EXPECT_GT(totalQuantity(fills, "RUNUP", 0), 0.0) << "RUNUP bought despite the outage";
    EXPECT_EQ(totalQuantity(fills, "RUNUP", 1), 0.0);
    EXPECT_GT(totalQuantity(fills, "SWING", 0), 0.0);
    EXPECT_DOUBLE_EQ(totalQuantity(fills, "SWING", 1), totalQuantity(fills, "SWING", 0)) << "SWING sold";
    EXPECT_EQ(totalQuantity(fills, "SLIDE", 0), 0.0);
}

} // namespace
