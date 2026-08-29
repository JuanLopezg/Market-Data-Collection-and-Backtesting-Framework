#pragma once

#include <string>
#include <vector>

#include "data_types.h"


/**************************************************************************************
 * Type    : ReconciliationIssueKind
 * Purpose : Machine-readable reason why local and exchange state disagree
 **************************************************************************************/
enum class ReconciliationIssueKind {
    CashMismatch,
    PositionMismatch,
    MissingExchangeOrder,
    UnexpectedExchangeOrder,
    OrderMismatch
};


/**************************************************************************************
 * Type    : ReconciliationIssue
 * Purpose : One blocking discrepancy found during startup reconciliation
 **************************************************************************************/
struct ReconciliationIssue {
    ReconciliationIssueKind kind = ReconciliationIssueKind::PositionMismatch;
    Coin coin;
    OrderID order_id = 0;
    double local_value = 0.0;
    double exchange_value = 0.0;
    std::string message;
};


/**************************************************************************************
 * Type    : ReconciliationReport
 * Purpose : Complete result of comparing persisted/local state against exchange truth
 **************************************************************************************/
struct ReconciliationReport {
    std::vector<ReconciliationIssue> issues;

    bool clean() const
    {
        return issues.empty();
    }
};
