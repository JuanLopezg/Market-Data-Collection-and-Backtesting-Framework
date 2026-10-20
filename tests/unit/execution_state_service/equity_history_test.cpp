// Daily equity rows of the accounting projection (tests/specs/execution_state_service.md, Part A).

#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "equity_history.h"

namespace {

constexpr double INITIAL_CASH = 10000.0;
constexpr double EPS = 1e-9;

Fill makeFill(FillID id, Timestamp timestamp, const Coin& coin, OrderSide side, double quantity, double price,
              double commission)
{
    Fill fill;
    fill.fill_id = id;
    fill.order_id = static_cast<OrderID>(id);
    fill.strategy_id = 1;
    fill.timestamp = timestamp;
    fill.coin = coin;
    fill.side = side;
    fill.quantity = quantity;
    fill.price = price;
    fill.commission = commission;
    return fill;
}

VenueCostEvent movement(VenueCostKind kind, Timestamp day, double amount)
{
    VenueCostEvent event;
    event.cost_id = "cost-" + std::to_string(day) + "-" + std::to_string(static_cast<int>(kind));
    event.kind = kind;
    event.timestamp = day;
    event.amount_usd = amount;
    return event;
}

FundingPayment funding(const std::string& id, Timestamp day, const Coin& coin, double amount)
{
    return FundingPayment{id, day, coin, amount};
}

// The worked example of spec A5: buy 0.1 BTC on day 12, sell 0.05 on day 13.
std::vector<Fill> exampleFills()
{
    return {makeFill(1, 20261012, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 3.0),
            makeFill(2, 20261013, "BTCUSDT", OrderSide::Sell, 0.05, 62500.0, 1.25)};
}

const std::map<Timestamp, double> BTC_CLOSES = {
    {20261012, 61000.0}, {20261013, 62000.0}, {20261014, 59000.0}, {20261015, 60000.0}};

// Closes from BTC_CLOSES; records what was asked for each day.
struct ClosesRecorder {
    std::map<Timestamp, std::set<Coin>> asked;
    std::set<Timestamp> missing_days;

    DayClosesLoader loader()
    {
        return [this](Timestamp day, const std::set<Coin>& coins) -> std::optional<std::unordered_map<Coin, double>> {
            asked[day] = coins;
            if (missing_days.contains(day))
                return std::nullopt;
            std::unordered_map<Coin, double> closes;
            for (const Coin& coin : coins) {
                if (coin != "BTCUSDT" || !BTC_CLOSES.contains(day))
                    return std::nullopt;
                closes[coin] = BTC_CLOSES.at(day);
            }
            return closes;
        };
    }
};

double noCollateral(Timestamp) { return 0.0; }

const EquityDay* rowFor(const std::vector<EquityDay>& rows, Timestamp day)
{
    for (const EquityDay& row : rows)
        if (row.date == day)
            return &row;
    return nullptr;
}

std::vector<Timestamp> datesOf(const std::vector<EquityDay>& rows)
{
    std::vector<Timestamp> dates;
    for (const EquityDay& row : rows)
        dates.push_back(row.date);
    return dates;
}

} // namespace


// ---------------------------------------------------------------- A1

TEST(EquityHistory, A1_TradingDayOfLiveAndReplayTimestamps)
{
    EXPECT_EQ(tradingDayOf(20261014u), 20261014u);
    EXPECT_EQ(tradingDayOf(202610143u), 20261014u);
    EXPECT_EQ(tradingDayOf(202612319u), 20261231u);
}

// ---------------------------------------------------------------- A2 / A3 / A4

TEST(EquityHistory, A2_EquityIsCashPlusSignedPositionsAtClose)
{
    const PositionLedger ledger;
    const EquityDay row = markEquityDay(20261014, 4000.0, {{"BTCUSDT", 0.1}, {"ETHUSDT", -1.0}, {"SOLUSDT", 0.0}},
                                        {{"BTCUSDT", 60000.0}, {"ETHUSDT", 2500.0}}, ledger, {}, 0.0);
    EXPECT_EQ(row.date, 20261014u);
    EXPECT_NEAR(row.cash, 4000.0, EPS);
    EXPECT_NEAR(row.positions_value, 3500.0, EPS);
    EXPECT_NEAR(row.equity, 7500.0, EPS);
}

TEST(EquityHistory, A2_CollateralIsRecordedButNotPartOfEquity)
{
    const PositionLedger ledger;
    const EquityDay row = markEquityDay(20261014, 4000.0, {{"BTCUSDT", 0.1}, {"ETHUSDT", -1.0}},
                                        {{"BTCUSDT", 60000.0}, {"ETHUSDT", 2500.0}}, ledger, {}, 500.0);
    EXPECT_NEAR(row.collateral_usd, 500.0, EPS);
    EXPECT_NEAR(row.equity, 7500.0, EPS);
}

TEST(EquityHistory, A3_RowCarriesLedgerTotals)
{
    PositionLedger ledger;
    for (const Fill& fill : exampleFills())
        ledger.applyFill(fill);
    const EquityDay row = markEquityDay(20261013, 7120.75, {{"BTCUSDT", 0.05}}, {{"BTCUSDT", 62000.0}}, ledger, {}, 0.0);
    EXPECT_NEAR(row.realized_gross_pnl, 125.0, 1e-6);
    EXPECT_NEAR(row.fees, 4.25, 1e-9);
    EXPECT_NEAR(row.funding, 0.0, 1e-12);
    EXPECT_EQ(row.open_positions, 1u);
}

TEST(EquityHistory, A4_InterestAndCapitalFlowsTotalledUpToTheDayOnly)
{
    const PositionLedger ledger;
    const std::vector<VenueCostEvent> movements = {movement(VenueCostKind::Interest, 20261012, -1.5),
                                                   movement(VenueCostKind::CapitalFlow, 20261013, 1000.0),
                                                   movement(VenueCostKind::Interest, 20261015, -0.5)};
    const EquityDay day13 = markEquityDay(20261013, 5000.0, {}, {}, ledger, movements, 0.0);
    EXPECT_NEAR(day13.interest, -1.5, EPS);
    EXPECT_NEAR(day13.capital_flows, 1000.0, EPS);
    EXPECT_NEAR(day13.equity, 5000.0, EPS) << "movements are already in cash";

    const EquityDay day15 = markEquityDay(20261015, 5000.0, {}, {}, ledger, movements, 0.0);
    EXPECT_NEAR(day15.interest, -2.0, EPS);
    EXPECT_NEAR(day15.capital_flows, 1000.0, EPS);
}

// ---------------------------------------------------------------- A5 / A6

TEST(EquityHistory, A5_RebuildGivesOneRowPerDayWithTheWorkedValues)
{
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(),
                                          noCollateral);
    ASSERT_EQ(datesOf(rows), (std::vector<Timestamp>{20261012, 20261013, 20261014}));

    EXPECT_NEAR(rows[0].cash, 3997.0, 1e-6);
    EXPECT_NEAR(rows[0].positions_value, 6100.0, 1e-6);
    EXPECT_NEAR(rows[0].equity, 10097.0, 1e-6);

    EXPECT_NEAR(rows[1].cash, 7120.75, 1e-6);
    EXPECT_NEAR(rows[1].positions_value, 3100.0, 1e-6);
    EXPECT_NEAR(rows[1].equity, 10220.75, 1e-6);
    EXPECT_NEAR(rows[1].realized_gross_pnl, 125.0, 1e-6);
    EXPECT_NEAR(rows[1].fees, 4.25, 1e-9);
    EXPECT_EQ(rows[1].open_positions, 1u);

    EXPECT_NEAR(rows[2].cash, 7120.75, 1e-6);
    EXPECT_NEAR(rows[2].positions_value, 2950.0, 1e-6);
    EXPECT_NEAR(rows[2].equity, 10070.75, 1e-6);
}

TEST(EquityHistory, A5_CalendarDaysContinueAcrossMonthEnd)
{
    const std::vector<Fill> fills = {makeFill(1, 20261030, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 0.0)};
    const auto loader = [](Timestamp, const std::set<Coin>& coins) -> std::optional<std::unordered_map<Coin, double>> {
        std::unordered_map<Coin, double> closes;
        for (const Coin& coin : coins)
            closes[coin] = 60000.0;
        return closes;
    };
    const auto rows = equityDaysFromFills(fills, {}, {}, INITIAL_CASH, 20261102, {}, loader, noCollateral);
    EXPECT_EQ(datesOf(rows), (std::vector<Timestamp>{20261030, 20261031, 20261101, 20261102}));
    for (const EquityDay& row : rows)
        EXPECT_NEAR(row.equity, INITIAL_CASH, 1e-6);
}

TEST(EquityHistory, A6_NextOpenFillBelongsToTheNextRow)
{
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(),
                                          noCollateral);
    const EquityDay* day12 = rowFor(rows, 20261012);
    ASSERT_NE(day12, nullptr);
    // The 20261013 sale (T+1 open) is not in close(20261012).
    EXPECT_NEAR(day12->cash, 3997.0, 1e-6);
    EXPECT_NEAR(day12->realized_gross_pnl, 0.0, 1e-12);
    EXPECT_NEAR(day12->fees, 3.0, 1e-12);
}

TEST(EquityHistory, A6_ClosesAreAskedForExactlyTheHeldCoins)
{
    // Day 12 buy BTC and ETH, day 13 sell all ETH: day 13 and 14 need only BTC.
    const std::vector<Fill> fills = {makeFill(1, 20261012, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 0.0),
                                     makeFill(2, 20261012, "ETHUSDT", OrderSide::Buy, 1.0, 2500.0, 0.0),
                                     makeFill(3, 20261013, "ETHUSDT", OrderSide::Sell, 1.0, 2600.0, 0.0)};
    std::map<Timestamp, std::set<Coin>> asked;
    const auto loader = [&](Timestamp day, const std::set<Coin>& coins) -> std::optional<std::unordered_map<Coin, double>> {
        asked[day] = coins;
        std::unordered_map<Coin, double> closes;
        for (const Coin& coin : coins)
            closes[coin] = coin == "BTCUSDT" ? 61000.0 : 2550.0;
        return closes;
    };
    const auto rows = equityDaysFromFills(fills, {}, {}, INITIAL_CASH, 20261014, {}, loader, noCollateral);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(asked[20261012], (std::set<Coin>{"BTCUSDT", "ETHUSDT"}));
    EXPECT_EQ(asked[20261013], (std::set<Coin>{"BTCUSDT"}));
    EXPECT_EQ(asked[20261014], (std::set<Coin>{"BTCUSDT"}));
    // Day 12: cash 10,000 - 6,000 - 2,500 = 1,500; BTC 6,100 + ETH 2,550 -> 10,150.
    EXPECT_NEAR(rows[0].equity, 10150.0, 1e-6);
    // Day 13: cash 1,500 + 2,600 = 4,100; BTC 6,100 -> 10,200; realized 100.
    EXPECT_NEAR(rows[1].equity, 10200.0, 1e-6);
    EXPECT_NEAR(rows[1].realized_gross_pnl, 100.0, 1e-6);
}

// ---------------------------------------------------------------- A7

TEST(EquityHistory, A7_ReplayTimestampsBelongToTheirBusinessDay)
{
    const std::vector<Fill> fills = {makeFill(1, 202610121, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 3.0),
                                     makeFill(2, 202610131, "BTCUSDT", OrderSide::Sell, 0.05, 62500.0, 1.25)};
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(fills, {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(), noCollateral);
    ASSERT_EQ(datesOf(rows), (std::vector<Timestamp>{20261012, 20261013, 20261014}));
    EXPECT_NEAR(rows[0].equity, 10097.0, 1e-6);
    EXPECT_NEAR(rows[1].equity, 10220.75, 1e-6);
}

// ---------------------------------------------------------------- A8 / A9 / A10

TEST(EquityHistory, A8_ExistingDaysAreSkippedButReplayedThrough)
{
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261014, {20261013},
                                          closes.loader(), noCollateral);
    ASSERT_EQ(datesOf(rows), (std::vector<Timestamp>{20261012, 20261014}));
    EXPECT_NEAR(rows[1].cash, 7120.75, 1e-6);
    EXPECT_NEAR(rows[1].equity, 10070.75, 1e-6);
    EXPECT_NEAR(rows[1].realized_gross_pnl, 125.0, 1e-6);
}

TEST(EquityHistory, A9_DayWithoutClosesIsLeftOut)
{
    ClosesRecorder closes;
    closes.missing_days = {20261013};
    const auto rows = equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(),
                                          noCollateral);
    ASSERT_EQ(datesOf(rows), (std::vector<Timestamp>{20261012, 20261014}));
    EXPECT_NEAR(rows[0].equity, 10097.0, 1e-6);
    EXPECT_NEAR(rows[1].equity, 10070.75, 1e-6);
}

TEST(EquityHistory, A10_NoFillsNoRows)
{
    ClosesRecorder closes;
    EXPECT_TRUE(equityDaysFromFills({}, {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(), noCollateral).empty());
}

TEST(EquityHistory, A10_LastDayBeforeFirstFillGivesNoRows)
{
    ClosesRecorder closes;
    EXPECT_TRUE(equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261011, {}, closes.loader(), noCollateral)
                    .empty());
}

// ---------------------------------------------------------------- A11

TEST(EquityHistory, A11_FundingAndCapitalFlowsMoveCashOnTheirDay)
{
    ClosesRecorder closes;
    const std::vector<FundingPayment> payments = {funding("f-13", 20261013, "BTCUSDT", -2.0)};
    const std::vector<VenueCostEvent> movements = {movement(VenueCostKind::CapitalFlow, 20261014, 1000.0)};
    const auto rows = equityDaysFromFills(exampleFills(), payments, movements, INITIAL_CASH, 20261014, {},
                                          closes.loader(), noCollateral);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NEAR(rows[0].cash, 3997.0, 1e-6) << "nothing from day 13 or 14 in day 12";
    EXPECT_NEAR(rows[0].capital_flows, 0.0, 1e-12);

    EXPECT_NEAR(rows[1].cash, 7118.75, 1e-6);
    EXPECT_NEAR(rows[1].funding, -2.0, 1e-9);
    EXPECT_NEAR(rows[1].capital_flows, 0.0, 1e-12);

    EXPECT_NEAR(rows[2].cash, 8118.75, 1e-6);
    EXPECT_NEAR(rows[2].capital_flows, 1000.0, 1e-9);
    EXPECT_NEAR(rows[2].equity, 11068.75, 1e-6);
}

TEST(EquityHistory, A11_CollateralIsAskedPerDay)
{
    ClosesRecorder closes;
    const auto collateral = [](Timestamp day) { return day == 20261013 ? 250.0 : 100.0; };
    const auto rows = equityDaysFromFills(exampleFills(), {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(),
                                          collateral);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NEAR(rows[1].collateral_usd, 250.0, 1e-12);
    EXPECT_NEAR(rows[1].equity, 10220.75, 1e-6) << "collateral is not in equity";
    EXPECT_NEAR(rows[2].collateral_usd, 100.0, 1e-12);
}

// ---------------------------------------------------------------- A12

TEST(EquityHistory, A12_FundingOnTheEntryDayIsInTheRowFundingTotal)
{
    const std::vector<Fill> fills = {makeFill(1, 20261012, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 3.0)};
    const std::vector<FundingPayment> payments = {funding("f-12", 20261012, "BTCUSDT", -2.0)};
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(fills, payments, {}, INITIAL_CASH, 20261012, {}, closes.loader(),
                                          noCollateral);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(rows[0].cash, 3995.0, 1e-6);
    EXPECT_NEAR(rows[0].equity, 10095.0, 1e-6);
    EXPECT_NEAR(rows[0].funding, -2.0, 1e-9);
}

TEST(EquityHistory, A12_EveryRowSatisfiesTheEconomicInvariant)
{
    const std::vector<FundingPayment> payments = {funding("f-13", 20261013, "BTCUSDT", -2.0),
                                                  funding("f-14", 20261014, "BTCUSDT", 0.5)};
    const std::vector<VenueCostEvent> movements = {movement(VenueCostKind::Interest, 20261013, -0.75),
                                                   movement(VenueCostKind::CapitalFlow, 20261014, 1000.0)};
    std::vector<Fill> fills = exampleFills();
    fills.push_back(makeFill(3, 20261015, "BTCUSDT", OrderSide::Buy, 0.02, 59500.0, 0.6));
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(fills, payments, movements, INITIAL_CASH, 20261015, {}, closes.loader(),
                                          noCollateral);
    ASSERT_EQ(rows.size(), 4u);

    // Average entry and held quantity per day, computed by hand from the fills.
    const std::map<Timestamp, std::pair<double, double>> held = {
        {20261012, {0.10, 60000.0}},
        {20261013, {0.05, 60000.0}},
        {20261014, {0.05, 60000.0}},
        {20261015, {0.07, (0.05 * 60000.0 + 0.02 * 59500.0) / 0.07}}};
    for (const EquityDay& row : rows) {
        const auto [quantity, entry] = held.at(row.date);
        const double unrealized = quantity * (BTC_CLOSES.at(row.date) - entry);
        const double left = row.equity - INITIAL_CASH - row.capital_flows - row.interest;
        const double right = row.realized_gross_pnl - row.fees + row.funding + unrealized;
        EXPECT_NEAR(left, right, 1e-6) << "day " << row.date;
    }
}

// ---------------------------------------------------------------- A13

TEST(EquityHistory, A13_DuplicateFillIdIsAppliedOnce)
{
    std::vector<Fill> fills = exampleFills();
    fills.insert(fills.begin() + 1, fills.front()); // fill 1 redelivered
    ClosesRecorder closes;
    const auto rows = equityDaysFromFills(fills, {}, {}, INITIAL_CASH, 20261014, {}, closes.loader(), noCollateral);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_NEAR(rows[0].cash, 3997.0, 1e-6);
    EXPECT_NEAR(rows[0].equity, 10097.0, 1e-6);
    EXPECT_NEAR(rows[2].equity, 10070.75, 1e-6);
}

// ---------------------------------------------------------------- A14

namespace {

struct ReachCase {
    std::vector<Fill> fills{makeFill(1, 20261012, "BTCUSDT", OrderSide::Buy, 0.1, 60000.0, 3.0)};
    std::vector<FundingPayment> payments{funding("f-12", 20261012, "BTCUSDT", -2.0)};
    std::vector<VenueCostEvent> movements{movement(VenueCostKind::Interest, 20261013, -1.0)};
};

Account liveAccount(double cash, const std::unordered_map<Coin, double>& positions)
{
    Account account(INITIAL_CASH);
    account.restoreState(cash, positions);
    return account;
}

} // namespace

TEST(EquityHistory, A14_FillsReachTheMatchingLiveAccount)
{
    const ReachCase c;
    EXPECT_TRUE(fillsReachAccount(c.fills, c.payments, c.movements, INITIAL_CASH,
                                  liveAccount(3994.0, {{"BTCUSDT", 0.1}})));
}

TEST(EquityHistory, A14_DifferentCashIsNotReached)
{
    const ReachCase c;
    EXPECT_FALSE(fillsReachAccount(c.fills, c.payments, c.movements, INITIAL_CASH,
                                   liveAccount(3900.0, {{"BTCUSDT", 0.1}})));
}

TEST(EquityHistory, A14_DifferentPositionIsNotReached)
{
    const ReachCase c;
    EXPECT_FALSE(fillsReachAccount(c.fills, c.payments, c.movements, INITIAL_CASH,
                                   liveAccount(3994.0, {{"BTCUSDT", 0.2}})));
    EXPECT_FALSE(fillsReachAccount(c.fills, c.payments, c.movements, INITIAL_CASH, liveAccount(3994.0, {})));
}

TEST(EquityHistory, A14_LivePositionNoFillExplainsIsNotReached)
{
    const ReachCase c;
    EXPECT_FALSE(fillsReachAccount(c.fills, c.payments, c.movements, INITIAL_CASH,
                                   liveAccount(3994.0, {{"BTCUSDT", 0.1}, {"ETHUSDT", 1.0}})));
}
