#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"

grep -q '#include "time_handler_factory.h"' "$FILE" \
  || fail "PortfolioRisk does not include the shared TimeHandler factory"
grep -q 'TimeHandlerFactory::loadConfigFromEnvironment()' "$FILE" \
  || fail "PortfolioRisk does not load canonical TimeHandler configuration"
grep -q 'TimeHandlerFactory::create(time_config_)' "$FILE" \
  || fail "PortfolioRisk does not construct TimeHandler from canonical config"

# Both message guards and the main daily gate must use the same business-time source.
COUNT=$(grep -c 'newestCompletedBusinessUtcDate(time_handler_)' "$FILE" || true)
[[ "$COUNT" -ge 3 ]] \
  || fail "PortfolioRisk does not consistently use TimeHandler for completed-day checks (found $COUNT, expected at least 3)"

grep -q 'computeNextMidnightUTC(time_handler_.getTime())' "$FILE" \
  || fail "PortfolioRisk next business UTC boundary is not based on TimeHandler"
grep -q 'interruptibleBusinessWaitUntil' "$FILE" \
  || fail "PortfolioRisk has no interruptible business-time wait"

if grep -q 'std::chrono::system_clock::now()' "$FILE"; then
  echo "Unexpected direct system_clock::now() uses:" >&2
  grep -n 'std::chrono::system_clock::now()' "$FILE" >&2
  fail "PortfolioRisk still reads host wall clock directly for runtime decisions"
fi

# The economic join must remain exact StrategyIntent(T) + AccountSnapshot(T).
grep -q 'const auto accountRow = checkpoint_store_->accountFor(target);' "$FILE" \
  || fail "PortfolioRisk exact account snapshot lookup for target T changed"
grep -q 'if (account.timestamp != target' "$FILE" \
  || fail "PortfolioRisk no longer validates AccountSnapshot(T) against StrategyIntent target T"

# Preserve established processing order: account consumer is polled before strategy consumer.
ACCOUNT_LINE=$(grep -n 'bus_\.poll(account_subscription_' "$FILE" | head -n1 | cut -d: -f1)
STRATEGY_LINE=$(grep -n 'bus_\.poll(strategy_subscription_' "$FILE" | head -n1 | cut -d: -f1)
[[ -n "$ACCOUNT_LINE" && -n "$STRATEGY_LINE" && "$ACCOUNT_LINE" -lt "$STRATEGY_LINE" ]] \
  || fail "PortfolioRisk account-before-strategy polling order changed"

# Technical cadence remains monotonic/real and is never speed-scaled.
grep -q 'std::chrono::steady_clock::now()' "$FILE" \
  || fail "PortfolioRisk technical loop no longer uses steady_clock"
grep -q 'std::this_thread::sleep_for' "$FILE" \
  || fail "PortfolioRisk technical polling sleep is missing"

# Legacy distributed clock must not be reintroduced here.
if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--simulation-id|--runtime-mode' "$FILE"; then
  echo "Legacy clock residue found in PortfolioRisk:" >&2
  grep -nE 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--simulation-id|--runtime-mode' "$FILE" >&2 || true
  fail "legacy distributed clock was reintroduced into PortfolioRisk"
fi

echo "PASS: T8 PortfolioRisk business/technical time separation audit"
