// Daily equity rows of ExecutionState's accounting projection (trading_equity_daily).
//
// A day's row is the account at close(T): after the fills of T's open execution and
// before those of T+1's, every position marked at its exact close(T). The fills (plus
// venue funding and account movements) are the authority; the rows are derived from them
// and are read by the dashboard (Overview equity curve, Close loop).
//
// These are plain functions over the data, without database or market-data access, so
// the rules can be tested on their own. execution_state_service_main.cpp loads the
// inputs and stores the rows.

#pragma once

#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "account.h"
#include "execution_messages.h"
#include "position_ledger.h"
#include "postgres_state_store.h"

// The business day of a fill/cost timestamp. Replay timestamps carry an extra phase digit
// (YYYYMMDDp); live ones are YYYYMMDD.
Timestamp tradingDayOf(Timestamp timestamp);

// The account marked at close(day). `closes` must hold a close for every nonzero position.
// Interest and capital flows up to that day (movements oldest first) and the day's venue
// collateral are recorded beside the equity; they are not part of it.
EquityDay markEquityDay(
    Timestamp day,
    double cash,
    const std::unordered_map<Coin, double>& positions,
    const std::unordered_map<Coin, double>& closes,
    const PositionLedger& ledger,
    const std::vector<VenueCostEvent>& accountMovements,
    double collateralUsd);

// True when replaying every fill, funding payment and account movement from the initial
// cash ends at the live account. Otherwise the state came from another source and the
// fills cannot rebuild its history.
bool fillsReachAccount(
    const std::vector<Fill>& fills,
    const std::vector<FundingPayment>& funding,
    const std::vector<VenueCostEvent>& accountMovements,
    double initialCash,
    const Account& live);

// Closes of `coins` at close(day), or nothing when the market data cannot provide them.
using DayClosesLoader = std::function<std::optional<std::unordered_map<Coin, double>>(
    Timestamp day, const std::set<Coin>& coins)>;

// Rows for the days from the first fill to lastDay that are not in `existing`, rebuilt by
// replaying the inputs from the initial cash: each day's row uses only the funding,
// movements and fills dated up to that day. A day without closes is left out.
std::vector<EquityDay> equityDaysFromFills(
    const std::vector<Fill>& fills,
    const std::vector<FundingPayment>& funding,
    const std::vector<VenueCostEvent>& accountMovements,
    double initialCash,
    Timestamp lastDay,
    const std::set<Timestamp>& existing,
    const DayClosesLoader& closesFor,
    const std::function<double(Timestamp day)>& collateralFor);
