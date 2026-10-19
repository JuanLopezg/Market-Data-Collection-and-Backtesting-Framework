package provider

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Alerts & Audit for the replay simulation.
//
// Alerts are notices derived from the current replay state; they never stop trading,
// close positions or change sizing. Leverage and drawdown use the same scales as the
// Overview and Risk pages. The audit log is the manual requests of this run with their
// outcome, as recorded by the replay in <simulation dir>/processed.

const (
	alertLeverageWarn     = 1.5
	alertLeverageCritical = 2.5
	alertDrawdownWarn     = -10.0
	alertDrawdownCritical = -25.0
)

func (p *Simulation) alertsAudit(s simulationSnapshot) map[string]any {
	alerts := simulationAlerts(s)
	critical, elevated, warn := 0, 0, 0
	for _, a := range alerts {
		switch a["severity"] {
		case "CRITICAL":
			critical++
		case "ALERT":
			elevated++
		case "WARN":
			warn++
		}
	}
	return map[string]any{
		"activeCritical": critical, "activeAlerts": elevated, "activeWarnings": warn, "acknowledged": 0, "resolved24h": 0,
		"alerts": alerts, "audit": p.manualAuditLog(), "sourceMode": "MOCK",
		"sourceUpdatedAt": s.HistoricalDate, "acknowledgementAvailable": false,
		"durableAlertHistoryAvailable": false, "humanAuditAvailable": true,
		"auditMode": "SIMULATION_MANUAL_REQUESTS", "watchdogAvailable": false, "watchdogState": "NOT_WIRED",
		"durableEventCount": 0, "durableLifecycleEvents": []any{}, "acknowledgementEventCount": 0,
	}
}

func simulationAlert(id, severity, source, title, detail, since string) map[string]any {
	return map[string]any{"id": id, "timestamp": since, "severity": severity, "status": "ACTIVE",
		"service": source, "eventType": id, "title": title, "detail": detail}
}

func simulationAlerts(s simulationSnapshot) []map[string]any {
	alerts := []map[string]any{}
	today := s.HistoricalDate

	if !s.RouteSafe {
		alerts = append(alerts, simulationAlert("ROUTE_UNSAFE", "CRITICAL", "Execution",
			"New orders are blocked", "The venue was unavailable or delivered data out of order; the replay stopped routing new orders.", today))
	}
	if s.Reconciliation.State != "CLEAN" {
		severity := "WARN"
		if s.Reconciliation.State == "BLOCKED" {
			severity = "CRITICAL"
		}
		alerts = append(alerts, simulationAlert("RECONCILIATION", severity, "Reconciliation",
			"Account does not match the venue ("+strings.ToLower(s.Reconciliation.State)+")", strings.Join(reconciliationMessages(s), "; "), today))
	}

	history := s.EquityHistory
	if n := len(history); n > 0 && history[n-1].Gross != nil && history[n-1].Equity > 0 {
		leverage := func(point simulationEquityPoint) float64 {
			if point.Gross == nil || point.Equity <= 0 {
				return 0
			}
			return *point.Gross / point.Equity
		}
		current := leverage(history[n-1])
		if severity, limit := levelAbove(current, alertLeverageWarn, alertLeverageCritical); severity != "" {
			since := streakStart(history, func(point simulationEquityPoint) bool { return leverage(point) > limit })
			alerts = append(alerts, simulationAlert("LEVERAGE", severity, "Risk",
				fmt.Sprintf("Leverage %.2fx", current), fmt.Sprintf("Above %.1fx since %s. Notice only: sizing is unchanged.", limit, since), since))
		}
	}

	if len(history) > 0 {
		peak := 0.0
		drawdowns := make([]float64, len(history))
		for i, point := range history {
			peak = math.Max(peak, point.Equity)
			if peak > 0 {
				drawdowns[i] = (point.Equity/peak - 1) * 100
			}
		}
		current := drawdowns[len(drawdowns)-1]
		if severity, limit := levelBelow(current, alertDrawdownWarn, alertDrawdownCritical); severity != "" {
			start := len(drawdowns) - 1
			for start > 0 && drawdowns[start-1] < limit {
				start--
			}
			since := history[start].Label
			alerts = append(alerts, simulationAlert("DRAWDOWN", severity, "Risk",
				fmt.Sprintf("Drawdown %.1f%% from peak", current), fmt.Sprintf("Below %.0f%% since %s. Notice only: nothing is closed.", limit, since), since))
		}
	}

	rejected := []string{}
	var firstRejected uint64
	for _, order := range s.Orders {
		if order.Status == "REJECTED" {
			rejected = append(rejected, order.Asset)
			if firstRejected == 0 || order.ActiveFrom < firstRejected {
				firstRejected = order.ActiveFrom
			}
		}
	}
	if len(rejected) > 0 {
		sort.Strings(rejected)
		alerts = append(alerts, simulationAlert("REJECTED_ORDERS", "WARN", "Execution",
			fmt.Sprintf("%d rejected orders", len(rejected)), "Assets: "+strings.Join(uniqueStrings(rejected), ", "), formatTradingTimestamp(firstRejected/10)))
	}

	if s.Manual.LastStatus == "REJECTED" {
		alerts = append(alerts, simulationAlert("MANUAL_REJECTED", "WARN", "Manual Control",
			"Manual request rejected", s.Manual.LastDetail, today))
	}
	if s.Manual.ResyncPending {
		alerts = append(alerts, simulationAlert("MANUAL_REBALANCE", "INFO", "Manual Control",
			"Manual rebalance in place", "Executed at the "+s.Manual.LastExecuted+"; the end-of-day decision returns to the strategy targets.", s.Manual.LastExecuted))
	}
	return alerts
}

// levelAbove returns the severity of value against two rising thresholds and the
// threshold it crossed.
func levelAbove(value, warn, critical float64) (string, float64) {
	switch {
	case value > critical:
		return "CRITICAL", critical
	case value > warn:
		return "WARN", warn
	}
	return "", 0
}

func levelBelow(value, warn, critical float64) (string, float64) {
	switch {
	case value < critical:
		return "CRITICAL", critical
	case value < warn:
		return "WARN", warn
	}
	return "", 0
}

// First label of the trailing run of points matching the condition.
func streakStart(history []simulationEquityPoint, match func(simulationEquityPoint) bool) string {
	start := len(history) - 1
	for start > 0 && match(history[start-1]) {
		start--
	}
	return history[start].Label
}

// manualAuditLog lists processed manual requests, newest first.
func (p *Simulation) manualAuditLog() []map[string]any {
	dir := filepath.Join(p.cfg.Dir, "processed")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return []map[string]any{}
	}
	names := []string{}
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".request") {
			names = append(names, entry.Name())
		}
	}
	sort.Sort(sort.Reverse(sort.StringSlice(names))) // names start with the queue time
	rows := []map[string]any{}
	for _, name := range names {
		fields, targets := readManualRecord(filepath.Join(dir, name))
		detail := strings.Join(targets, ", ")
		if detail == "" {
			detail = "100% cash"
		}
		result := "SUCCESS"
		switch fields["result"] {
		case "REJECTED":
			result, detail = "REJECTED", fields["detail"]
		case "SUPERSEDED":
			result, detail = "NOOP", fields["detail"]
		}
		action := "Manual rebalance"
		rows = append(rows, map[string]any{
			"id": fields["request_id"], "timestamp": fields["replay_date"], "actor": fields["actor"], "actorType": "HUMAN",
			"action": action, "target": "portfolio", "result": result, "requestHash": fields["request_hash"],
			"correlationId": fields["request_id"], "detail": detail,
		})
	}
	return rows
}

func readManualRecord(path string) (map[string]string, []string) {
	fields := map[string]string{}
	targets := []string{}
	file, err := os.Open(path)
	if err != nil {
		return fields, targets
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		key, value, ok := strings.Cut(scanner.Text(), "=")
		if !ok {
			continue
		}
		if key == "target" {
			asset, weight, _ := strings.Cut(value, ",")
			var fraction float64
			fmt.Sscan(weight, &fraction)
			targets = append(targets, fmt.Sprintf("%s %.1f%%", asset, fraction*100))
			continue
		}
		fields[key] = value
	}
	return fields, targets
}
