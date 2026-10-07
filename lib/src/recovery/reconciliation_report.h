#pragma once

#include <string>
#include <vector>

#include "data_types.h"


// Machine-readable reason why local and exchange state disagree
enum class ReconciliationIssueKind {
    CashMismatch,
    PositionMismatch,
    MissingExchangeOrder,
    UnexpectedExchangeOrder,
    OrderMismatch
};


// One blocking discrepancy found during startup reconciliation
struct ReconciliationIssue {
    ReconciliationIssueKind kind = ReconciliationIssueKind::PositionMismatch;
    Coin coin;
    OrderID order_id = 0;
    double local_value = 0.0;
    double exchange_value = 0.0;
    std::string message;
};


// Complete result of comparing persisted/local state against exchange truth
// A report describes differences only. Repair or resubmission belongs to the
// recovery workflow after the caller decides whether trading can resume.
struct ReconciliationReport {
    std::vector<ReconciliationIssue> issues;

    bool clean() const
    {
        return issues.empty();
    }
};
