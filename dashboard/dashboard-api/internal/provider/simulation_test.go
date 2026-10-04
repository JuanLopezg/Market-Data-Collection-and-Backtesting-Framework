package provider

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeSimulationFixture(t *testing.T, dir string) simulationSnapshot {
	t.Helper()
	s := simulationSnapshot{
		SchemaVersion:            1,
		Generation:               7,
		Phase:                    "MANUAL_READY",
		HistoricalDate:           "2020-01-02",
		BusinessTimestamp:        202001029,
		DayIndex:                 2,
		DayCount:                 2,
		SourceRowsProcessed:      10,
		VenueID:                  "MOCK",
		Environment:              "MOCK",
		ReplaySpeed:              1000,
		RouteSafe:                true,
		Reconciliation:           simulationReconciliation{State: "CLEAN", LocalSequence: 4, VenueSequence: 4, LedgerHeadHash: strings.Repeat("a", 64), Issues: []simulationReconciliationIssue{}},
		Account:                  simulationAccount{Equity: 100000, MarginUsed: 0, CashTotal: 100000, CashAvailable: 100000, Positions: []simulationPosition{}},
		Orders:                   []simulationOrder{},
		Fills:                    []simulationFill{},
		Accounting:               []simulationAccounting{},
		Market:                   []simulationMarketBar{{Asset: "BTCUSDT", Open: 100, High: 101, Low: 99, Close: 100, Volume: 1000}},
		EquityHistory:            []simulationEquityPoint{{Label: "2020-01-02", Equity: 100000}},
		Ledger:                   simulationLedger{Valid: true, Error: "", HeadHash: strings.Repeat("a", 64), Entries: []simulationLedgerEntry{}},
		RoutableAssets:           []string{"BTCUSDT"},
		Manual:                   simulationManualState{Ready: true, DecisionTimestamp: 202001039, ExecutionTimestamp: 202001040},
		EconomicFingerprint:      "1234567890abcdef",
		StreamFingerprint:        "fedcba0987654321",
		Step56RuntimeFingerprint: strings.Repeat("b", 64),
		Step57ManualFingerprint:  strings.Repeat("c", 64),
	}
	raw, err := json.Marshal(s)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "state.json"), raw, 0o600); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestSimulationProviderReadsCanonicalSnapshot(t *testing.T) {
	dir := t.TempDir()
	writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})

	h := p.Health(context.Background())
	if !h.Ready || h.Mode != "mock" {
		t.Fatalf("unexpected health: %+v", h)
	}

	for _, resource := range AllResources {
		raw, err := p.Read(context.Background(), resource)
		if err != nil {
			t.Fatalf("read %s: %v", resource, err)
		}
		if !json.Valid(raw) {
			t.Fatalf("resource %s returned invalid JSON", resource)
		}
	}

	ledger := p.LedgerStatus(context.Background())
	if !ledger.Validated || !ledger.ReadOnly || ledger.OrderRouting != "MOCK_ONLY" {
		t.Fatalf("unexpected ledger: %+v", ledger)
	}
	venue := p.VenueFoundation(context.Background())
	if !venue.FoundationReady || venue.Venue != "MOCK" || venue.PrivateAuth != "DISABLED" {
		t.Fatalf("unexpected venue foundation: %+v", venue)
	}
}

func TestSimulationManualPreviewAndRouteQueuesFile(t *testing.T) {
	dir := t.TempDir()
	s := writeSimulationFixture(t, dir)
	p := NewSimulation(SimulationConfig{Dir: dir})
	csv := "asset,weight_pct\nBTCUSDT,10\nCASH,90\n"

	preview, err := p.PreviewManualControl(context.Background(), ManualControlPreviewRequest{Actor: "juan / OPERATOR", Filename: "manual.csv", CSV: csv})
	if err != nil {
		t.Fatal(err)
	}
	if !preview.ValidationPassed || !preview.RouteEnabled || preview.RequestHash == "" {
		t.Fatalf("unexpected preview: %+v", preview)
	}
	if preview.CurrentTargetTimestamp != "202001029" {
		t.Fatalf("timestamp: %s", preview.CurrentTargetTimestamp)
	}

	result, err := p.RouteManualControl(context.Background(), SimulationManualRouteRequest{
		Actor: "juan / OPERATOR", Filename: "manual.csv", CSV: csv,
		RequestHash: preview.RequestHash, ReferenceTargetTimestamp: preview.CurrentTargetTimestamp,
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Status != "ACCEPTED" || result.Submitted || !result.AuditPersisted || result.CorrelationID == "" {
		t.Fatalf("unexpected route result: %+v", result)
	}

	requestPath := filepath.Join(dir, "requests", result.CorrelationID+".request")
	raw, err := os.ReadFile(requestPath)
	if err != nil {
		t.Fatal(err)
	}
	body := string(raw)
	for _, want := range []string{
		"STEP58_MANUAL_V1\n",
		"correlation_id=" + result.CorrelationID + "\n",
		"request_hash=" + preview.RequestHash + "\n",
		"decision_timestamp=202001039\n",
		"execution_timestamp=202001040\n",
		"reference_generation=7\n",
		"target=BTCUSDT,0.100000000000\n",
		"cash=0.900000000000\n",
	} {
		if !strings.Contains(body, want) {
			t.Fatalf("request missing %q:\n%s", want, body)
		}
	}

	// A stale target timestamp must fail closed and must not create another request.
	stale, err := p.RouteManualControl(context.Background(), SimulationManualRouteRequest{
		Actor: "juan / OPERATOR", Filename: "manual.csv", CSV: csv,
		RequestHash: preview.RequestHash, ReferenceTargetTimestamp: "202001019",
	})
	if err != nil {
		t.Fatal(err)
	}
	if stale.Status != "BLOCKED" || stale.Submitted {
		t.Fatalf("stale request did not block: %+v", stale)
	}
	if s.Manual.Ready != true {
		t.Fatal("fixture invalid")
	}
}

func TestSimulationManualCSVRejectsDuplicateCash(t *testing.T) {
	_, _, err := parseSimulationManualCSV("asset,weight_pct\nCASH,50\nCASH,50\n")
	if err == nil {
		t.Fatal("duplicate CASH must fail")
	}
}
