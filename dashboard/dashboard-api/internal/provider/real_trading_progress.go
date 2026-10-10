package provider

import (
	"fmt"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

// This is an observation policy for the daily PAPER schedule, not an economic clock.
// Persisted execution timestamps prove plan application, including empty plans;
// they do not prove fills, reconciliation, or application responsiveness.
type realTradingProgress struct {
	State             string `json:"state"`
	Detail            string `json:"detail"`
	ExpectedCloseDate string `json:"expectedCloseDate"`
	ExpectedOpenDate  string `json:"expectedOpenDate"`
	LastDecisionDate  string `json:"lastDecisionDate"`
	LastPlanDate      string `json:"lastPlanDate"`
	PersistedAt       string `json:"persistedAt"`
}

func buildTradingProgress(runtime pgstore.RuntimeSummary, queryErr error, reachable bool, mode string, now time.Time) *realTradingProgress {
	result := &realTradingProgress{State: "UNKNOWN", Detail: "Daily wall-clock freshness applies only to current-data PAPER; historical business time is not compared with today's date."}
	if mode != "PAPER" {
		return result
	}
	today := now.UTC().Truncate(24 * time.Hour)
	expectedClose := today.AddDate(0, 0, -1)
	result.ExpectedCloseDate = expectedClose.Format("2006-01-02")
	result.ExpectedOpenDate = today.Format("2006-01-02")
	if !reachable || queryErr != nil {
		result.State = "CRITICAL"
		result.Detail = "Durable trading progress unavailable: authenticated PostgreSQL read failed."
		return result
	}
	if !runtime.Present {
		result.Detail = "No durable execution state yet; no completed decision or applied plan can be asserted."
		return result
	}
	result.PersistedAt = runtime.UpdatedAt
	decision, decisionValid := parseMarketDate(runtime.LastBarCloseTimestamp)
	plan, planValid := parseMarketDate(runtime.LastExecutionTimestamp)
	if decisionValid {
		result.LastDecisionDate = decision.Format("2006-01-02")
	}
	if planValid {
		result.LastPlanDate = plan.Format("2006-01-02")
	}
	if (decisionValid && decision.After(expectedClose)) || (planValid && plan.After(today)) {
		result.State = "CRITICAL"
		result.Detail = "Persisted business dates are ahead of the completed daily PAPER window; check clock and timestamp ordering."
		return result
	}
	if decisionValid && planValid && decision.Equal(expectedClose) && plan.Equal(today) {
		result.State = "HEALTHY"
		result.Detail = "Latest completed-day decision and next-open plan application are current. Zero orders is valid; fills and reconciliation are separate evidence."
		if runtime.PendingPlanCount > 0 {
			result.State = "WARN"
			result.Detail += fmt.Sprintf(" %d persisted plans remain pending.", runtime.PendingPlanCount)
		}
		return result
	}
	// Allow ingestion/planning after UTC rollover without treating normal daily idle
	// time or the absence of fills as a failed service. This grace is explicit in UI.
	if now.UTC().Before(today.Add(30*time.Minute)) && decisionValid && planValid &&
		!decision.Before(expectedClose.AddDate(0, 0, -1)) && !plan.Before(today.AddDate(0, 0, -1)) {
		result.State = "WARN"
		result.Detail = "Waiting for the new daily cycle during the 30-minute UTC rollover grace; the previous daily cycle is retained."
		return result
	}
	result.State = "CRITICAL"
	result.Detail = "Daily decision or plan application is missing/behind the expected dates. Persistence time and recent fills do not replace business-cycle progress; check ingestion, planning and service processes."
	return result
}
