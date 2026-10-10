package provider

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestPaperComparisonMissingAndStale(t *testing.T) {
	path := filepath.Join(t.TempDir(), "comparison.json")
	if readPaperComparison(path).Status != "UNAVAILABLE" {
		t.Fatal("missing artifact must be unavailable")
	}
	value := paperComparison{ContractVersion: "paper-comparison-v1", Status: "OBSERVING", CheckedAt: time.Now().UTC().Format(time.RFC3339), Rows: []paperComparisonRow{{LiveEquity: 100, ExpectedEquity: 99}}}
	raw, _ := json.Marshal(value)
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	if got := readPaperComparison(path); got.Status != "OBSERVING" || len(got.Rows) != 1 {
		t.Fatal(got)
	}
	value.CheckedAt = time.Now().Add(-4 * time.Minute).UTC().Format(time.RFC3339)
	raw, _ = json.Marshal(value)
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	if readPaperComparison(path).Status != "STALE" {
		t.Fatal("old collector must not look live")
	}
}

func TestPaperThresholdDiagnosticPreservesStrictBoundaries(t *testing.T) {
	cfg := RealConfig{RSIEntry: 50, RSIExit: 40}
	_, state, _, _, _ := describeMarketSignal(50, 0, true, 0, cfg)
	if state != "WAIT" {
		t.Fatal(state)
	}
	_, state, _, _, _ = describeMarketSignal(50.01, 0, true, 0, cfg)
	if state != "CHECK" {
		t.Fatal(state)
	}
	_, state, _, _, _ = describeMarketSignal(39.99, 0, true, 0, cfg)
	if state != "FLAT" {
		t.Fatal(state)
	}
}
