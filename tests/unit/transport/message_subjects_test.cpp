#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "message_subjects.h"

namespace transport_test {
namespace {

std::set<std::string> subjectSet(const std::vector<std::string>& subjects)
{
    return {subjects.begin(), subjects.end()};
}

TEST(MessageSubjectsBehaviour, R11_RuntimeOwnsExactlyTheTradingProtocolSubjects)
{
    const std::set<std::string> expected{
        "market.release.v1", "market.data.updated.v1", "market.slice.closed.v1", "market.slice.snapshot.v1",
        "strategy.intents.v1", "decision.batch.v1", "execution.prices.v1",
        "execution.command.submit.v1", "execution.command.cancel.v1", "execution.event.order_update.v1",
        "execution.event.fill.v1", "execution.event.venue_cost.v1", "execution.account.snapshot.v1",
        "execution.cycle.complete.v1", "execution.exchange.snapshot.v1", "execution.plan.request.v1",
        "execution.plan.v1", "execution.plan.notional.request.v1", "execution.plan.notional.v1"
    };
    const auto trading = MessageSubjects::tradingRuntimeSubjects();
    EXPECT_EQ(trading.size(), 19U);
    EXPECT_EQ(subjectSet(trading), expected);
    EXPECT_EQ(MessageSubjects::runtimeSubjects(), trading);
}

TEST(MessageSubjectsBehaviour, R11_ControlAndBackendSubjectsHaveSeparateOwnership)
{
    const std::set<std::string> control{"execution.exchange.snapshot.request.v1"};
    const std::set<std::string> backend{
        "gateway.backend.command.submit.v1", "gateway.backend.command.cancel.v1",
        "gateway.backend.snapshot.request.v1", "gateway.backend.event.order_update.v1",
        "gateway.backend.event.fill.v1", "gateway.backend.snapshot.v1"
    };
    const auto controlActual = MessageSubjects::exchangeGatewayControlSubjects();
    const auto backendActual = MessageSubjects::exchangeBackendSubjects();
    EXPECT_EQ(controlActual.size(), 1U);
    EXPECT_EQ(backendActual.size(), 6U);
    EXPECT_EQ(subjectSet(controlActual), control);
    EXPECT_EQ(subjectSet(backendActual), backend);
    auto all = MessageSubjects::tradingRuntimeSubjects();
    all.insert(all.end(), controlActual.begin(), controlActual.end());
    all.insert(all.end(), backendActual.begin(), backendActual.end());
    EXPECT_EQ(all.size(), 26U);
    EXPECT_EQ(subjectSet(all).size(), 26U);
}

TEST(MessageSubjectsBehaviour, R12_ExecutionEventWildcardDoesNotIncludeCommandsOrAccounts)
{
    EXPECT_STREQ(MessageSubjects::EXECUTION_EVENT_FILTER, "execution.event.>");
    for (const char* subject : {MessageSubjects::ORDER_UPDATE, MessageSubjects::FILL, MessageSubjects::VENUE_COST})
        EXPECT_TRUE(std::string(subject).starts_with("execution.event."));
    for (const char* subject : {MessageSubjects::SUBMIT_ORDER, MessageSubjects::CANCEL_ORDER,
            MessageSubjects::ACCOUNT_SNAPSHOT, MessageSubjects::EXECUTION_CYCLE_COMPLETE,
            MessageSubjects::BACKEND_FILL, MessageSubjects::BACKEND_ORDER_UPDATE})
        EXPECT_FALSE(std::string(subject).starts_with("execution.event."));
}

} // namespace
} // namespace transport_test
