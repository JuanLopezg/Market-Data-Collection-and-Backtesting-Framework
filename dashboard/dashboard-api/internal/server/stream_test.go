package server

import (
	"slices"
	"testing"

	natsdiag "control-dashboard-api/internal/integration/nats"
)

func TestDashboardResourcesForFillInvalidatesClosedLoopViews(t *testing.T) {
	got := dashboardResourcesForSubject(natsdiag.SubjectFill)
	for _, want := range []string{"overview", "positions", "reconciliation", "pipeline", "execution", "alerts-audit", "shell-status"} {
		if !slices.Contains(got, want) {
			t.Fatalf("fill invalidations = %#v; missing %q", got, want)
		}
	}
}

func TestDashboardResourcesForMarketUpdateIncludesStrategyDiagnostics(t *testing.T) {
	got := dashboardResourcesForSubject(natsdiag.SubjectMarketDataUpdated)
	for _, want := range []string{"market-data", "live-vs-expected", "pipeline", "overview", "shell-status"} {
		if !slices.Contains(got, want) {
			t.Fatalf("market invalidations = %#v; missing %q", got, want)
		}
	}
}

func TestDashboardResourcesUnknownSubjectFailsSafeToShell(t *testing.T) {
	got := dashboardResourcesForSubject("unknown.subject.v1")
	if len(got) != 1 || got[0] != "shell-status" {
		t.Fatalf("unknown invalidations = %#v", got)
	}
}
