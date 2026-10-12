package provider

import (
	"context"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func manualRequestFiles(t *testing.T, dir string) []string {
	t.Helper()
	entries, err := os.ReadDir(filepath.Join(dir, "requests"))
	if err != nil {
		return nil
	}
	bodies := []string{}
	for _, entry := range entries {
		if strings.HasSuffix(entry.Name(), ".request") {
			raw, err := os.ReadFile(filepath.Join(dir, "requests", entry.Name()))
			if err != nil {
				t.Fatal(err)
			}
			bodies = append(bodies, string(raw))
		}
	}
	return bodies
}

func TestSimulationManualPreviewAndRouteQueuesRequest(t *testing.T) {
	dir := t.TempDir()
	writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})
	csv := "asset,weight_pct\nBTCUSDT,10\n" // CASH omitted: the rest stays in cash

	preview, err := p.PreviewManualControl(context.Background(), ManualControlPreviewRequest{Actor: "juan / OPERATOR", CSV: csv})
	if err != nil {
		t.Fatal(err)
	}
	if !preview.ValidationPassed || !preview.RouteEnabled || preview.RequestHash == "" {
		t.Fatalf("unexpected preview: %+v", preview)
	}
	if len(preview.PreviewRows) != 2 || preview.PreviewRows[0].Action != "BUY" || preview.PreviewRows[1].Asset != "CASH" || preview.PreviewRows[1].RequestedWeightPct != 90 {
		t.Fatalf("unexpected preview rows: %+v", preview.PreviewRows)
	}

	result, err := p.RouteManualControl(context.Background(), SimulationManualRouteRequest{Actor: "juan / OPERATOR", CSV: csv, RequestHash: preview.RequestHash})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != "QUEUED" || result.CorrelationID == "" {
		t.Fatalf("unexpected route result: %+v", result)
	}
	bodies := manualRequestFiles(t, dir)
	if len(bodies) != 1 {
		t.Fatalf("expected one queued request, got %d", len(bodies))
	}
	for _, want := range []string{"REPLAY_MANUAL_V1\n", "request_id=" + result.CorrelationID + "\n", "action=SET\n", "target=BTCUSDT,0.100000000000\n"} {
		if !strings.Contains(bodies[0], want) {
			t.Fatalf("request missing %q:\n%s", want, bodies[0])
		}
	}

	// A different CSV than the previewed one must not be queued.
	mismatch, err := p.RouteManualControl(context.Background(), SimulationManualRouteRequest{Actor: "juan / OPERATOR", CSV: "asset,weight_pct\nBTCUSDT,20\n", RequestHash: preview.RequestHash})
	if err != nil {
		t.Fatal(err)
	}
	if mismatch.Status != "BLOCKED" || len(manualRequestFiles(t, dir)) != 1 {
		t.Fatalf("hash mismatch was not blocked: %+v", mismatch)
	}

	base, err := p.manualBase(writeSimulationFixture(t, dir), "juan")
	if err != nil {
		t.Fatal(err)
	}
	if len(base.Requests) != 1 || base.Requests[0].ID != result.CorrelationID || base.Requests[0].Stage != "QUEUED" {
		t.Fatalf("queued request status: %+v", base.Requests)
	}
}

func TestManualRequestStatusFollowsOrdersToTheExchange(t *testing.T) {
	dir := t.TempDir()
	s := writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})
	processed := filepath.Join(dir, "processed")
	if err := os.MkdirAll(processed, 0o755); err != nil {
		t.Fatal(err)
	}
	record := "REPLAY_MANUAL_V1\nrequest_id=manual-a\nactor=juan\nrequest_hash=x\naction=SET\ntarget=BTCUSDT,0.1\n" +
		"result=EXECUTED\ndetail=2 orders at the 2020-01-02 close\nreplay_date=2020-01-02\norders=7-8\n"
	if err := os.WriteFile(filepath.Join(processed, "0001-manual-a.request"), []byte(record), 0o600); err != nil {
		t.Fatal(err)
	}

	s.Manual.LastCorrelationID, s.Manual.ResyncPending = "manual-a", true
	s.Orders = []simulationOrder{{OrderID: "7", Status: "FILLED"}, {OrderID: "8", Status: "ACCEPTED"}}
	if got := p.manualRequestStatuses(s); got[0].Stage != "AT_EXCHANGE" || got[0].Orders != 2 || got[0].Filled != 1 {
		t.Fatalf("partially filled: %+v", got)
	}
	s.Orders[1].Status = "FILLED"
	if got := p.manualRequestStatuses(s); got[0].Stage != "FILLED" {
		t.Fatalf("filled: %+v", got)
	}
	s.Manual.ResyncPending = false // the end-of-day decision ran
	if got := p.manualRequestStatuses(s); got[0].Stage != "RESYNCED" {
		t.Fatalf("resynced: %+v", got)
	}
}

func TestManualTargetCSVValidation(t *testing.T) {
	routable := map[string]bool{"BTCUSDT": true, "ETHUSDT": true}
	limits := manualLimits{grossPct: 150, assetPct: 150}
	cases := []struct {
		name, csv, wantError string
	}{
		{"duplicate cash", "asset,weight_pct\nCASH,50\nCASH,50\n", "CASH appears twice"},
		{"unknown short name", "asset,weight_pct\nBTC,10\n", "Did you mean BTCUSDT"},
		{"gross above risk limit", "asset,weight_pct\nBTCUSDT,100\nETHUSDT,60\n", "risk limit is 150%"},
		{"asset above risk limit", "asset,weight_pct\nBTCUSDT,160\n", "above the risk limit of 150%"},
		{"cash mismatch", "asset,weight_pct\nBTCUSDT,50\nCASH,40\n", "must sum to 100%"},
		{"bad header", "coin,weight\nBTCUSDT,10\n", "first line"},
	}
	for _, c := range cases {
		_, _, issues := parseManualTargetCSV(c.csv, routable, limits)
		found := false
		for _, issue := range issues {
			if issue.Severity == "ERROR" && strings.Contains(issue.Message, c.wantError) {
				found = true
			}
		}
		if !found {
			t.Fatalf("%s: expected error %q, got %+v", c.name, c.wantError, issues)
		}
	}

	// Leverage inside the risk limit borrows cash.
	targets, cash, issues := parseManualTargetCSV("asset,weight_pct\nBTCUSDT,80\nETHUSDT,40\n", routable, limits)
	if hasManualError(issues) || len(targets) != 2 || math.Abs(cash+20) > 1e-9 {
		t.Fatalf("leveraged csv: targets=%v cash=%v issues=%+v", targets, cash, issues)
	}

	// Excel (Spanish locale) export: BOM, ";" separator and decimal comma.
	targets, cash, issues = parseManualTargetCSV("\uFEFFasset;weight_pct\r\nbtcusdt;12,5\r\nETHUSDT;7,5%\r\n", routable, limits)
	if hasManualError(issues) || targets["BTCUSDT"] != 12.5 || targets["ETHUSDT"] != 7.5 || cash != 80 {
		t.Fatalf("excel csv: targets=%v cash=%v issues=%+v", targets, cash, issues)
	}
}
