package provider

import "testing"

func TestSimulationAlertsFollowRiskScales(t *testing.T) {
	gross := func(v float64) *float64 { return &v }
	s := simulationSnapshot{
		HistoricalDate: "2020-01-04", RouteSafe: true,
		Reconciliation: simulationReconciliation{State: "CLEAN"},
		EquityHistory: []simulationEquityPoint{
			{Label: "2020-01-01", Equity: 100000, Gross: gross(100000)}, // 1.0x
			{Label: "2020-01-02", Equity: 90000, Gross: gross(180000)},  // 2.0x, -10.0%
			{Label: "2020-01-03", Equity: 85000, Gross: gross(170000)},  // 2.0x, -15.0%
			{Label: "2020-01-04", Equity: 80000, Gross: gross(240000)},  // 3.0x, -20.0%
		},
		Manual: simulationManualState{ResyncPending: true, LastExecuted: "2020-01-04 close"},
	}
	byID := map[string]map[string]any{}
	for _, alert := range simulationAlerts(s) {
		byID[alert["id"].(string)] = alert
	}

	leverage := byID["LEVERAGE"]
	if leverage == nil || leverage["severity"] != "CRITICAL" || leverage["timestamp"] != "2020-01-04" {
		t.Fatalf("leverage alert: %+v", leverage)
	}
	drawdown := byID["DRAWDOWN"]
	// -10.0% exactly is not below the -10% line, so the streak starts on the 3rd.
	if drawdown == nil || drawdown["severity"] != "WARN" || drawdown["timestamp"] != "2020-01-03" {
		t.Fatalf("drawdown alert: %+v", drawdown)
	}
	if manual := byID["MANUAL_REBALANCE"]; manual == nil || manual["severity"] != "INFO" {
		t.Fatalf("manual alert: %+v", manual)
	}
	if _, ok := byID["RECONCILIATION"]; ok {
		t.Fatal("clean reconciliation must not alert")
	}
}
