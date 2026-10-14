package provider

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/tradingwire"
)

func TestChainManualRequestFileFormat(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, manualRequestsSubdir), 0o755); err != nil {
		t.Fatal(err)
	}
	id, err := writeChainManualRequest(dir, "operator / OPERATOR\nx", "sha256:abc", map[string]float64{"ETHUSDT": 10, "BTCUSDT": 25})
	if err != nil {
		t.Fatal(err)
	}
	entries, _ := os.ReadDir(filepath.Join(dir, manualRequestsSubdir))
	if len(entries) != 1 || !strings.HasSuffix(entries[0].Name(), "-"+id+".request") {
		t.Fatalf("request files = %v", entries)
	}
	info, _ := entries[0].Info()
	if info.Mode().Perm() != chainManualRequestMode {
		t.Fatalf("mode = %v, execution-state reads it as another user", info.Mode().Perm())
	}
	raw, _ := os.ReadFile(filepath.Join(dir, manualRequestsSubdir, entries[0].Name()))
	lines := strings.Split(strings.TrimSpace(string(raw)), "\n")
	if lines[0] != "MANUAL_REQUEST_V1" || lines[1] != "request_id="+id || lines[2] != "actor=operator / OPERATOR x" ||
		lines[3] != "request_hash=sha256:abc" || !strings.HasPrefix(lines[4], "created_at=") {
		t.Fatalf("header lines = %q", lines[:5])
	}
	// Weights are fractions of equity, sorted by asset (execution-state parses them).
	if lines[5] != "target=BTCUSDT,0.250000000000" || lines[6] != "target=ETHUSDT,0.100000000000" || len(lines) != 7 {
		t.Fatalf("target lines = %q", lines[5:])
	}
	if _, err := time.Parse("2006-01-02T15:04:05Z", strings.TrimPrefix(lines[4], "created_at=")); err != nil {
		t.Fatalf("created_at: %v", err)
	}
}

func TestChainManualStatusFollowsOrdersToTheResync(t *testing.T) {
	executed := map[string]string{"result": "EXECUTED", "detail": "2 orders sent", "execution_day": "20261014", "orders": "40-41"}
	order := func(id uint64, status int, message string) tradingwire.PersistedTrackedOrder {
		return tradingwire.PersistedTrackedOrder{OrderID: id, Coin: "BTCUSDT", Status: status, LastMessage: message}
	}
	cases := []struct {
		name          string
		fields        map[string]string
		orders        []tradingwire.PersistedTrackedOrder
		lastExecution uint64
		stage         string
	}{
		{"rejected by execution-state", map[string]string{"result": "REJECTED", "detail": "Orders are still open"}, nil, 0, "FAILED"},
		{"record written, orders not yet in the state", executed, nil, 20261014, "EXECUTED"},
		{"one order still open", executed, []tradingwire.PersistedTrackedOrder{order(40, orderStatusFilled, ""), order(41, 2, "")}, 20261014, "AT_EXCHANGE"},
		{"all filled", executed, []tradingwire.PersistedTrackedOrder{order(40, orderStatusFilled, ""), order(41, orderStatusFilled, "")}, 20261014, "FILLED"},
		{"one rejected by the venue", executed, []tradingwire.PersistedTrackedOrder{order(40, orderStatusFilled, ""), order(41, orderStatusRejected, "MARKET_UNAVAILABLE")}, 20261014, "FAILED"},
		{"next daily cycle executed", executed, []tradingwire.PersistedTrackedOrder{order(40, orderStatusFilled, ""), order(41, orderStatusFilled, "")}, 20261015, "RESYNCED"},
		{"no order needed", map[string]string{"result": "EXECUTED", "execution_day": "20261014"}, nil, 20261014, "RESYNCED"},
	}
	for _, c := range cases {
		orders := map[uint64]tradingwire.PersistedTrackedOrder{}
		for _, o := range c.orders {
			orders[o.OrderID] = o
		}
		status := chainManualStatus(ManualRequestStatus{ID: "manual-1"}, c.fields, orders, c.lastExecution)
		if status.Stage != c.stage {
			t.Fatalf("%s: stage = %s (%s), want %s", c.name, status.Stage, status.Detail, c.stage)
		}
		if c.name == "one rejected by the venue" && !strings.Contains(status.Detail, "MARKET_UNAVAILABLE") {
			t.Fatalf("venue reason missing: %s", status.Detail)
		}
	}
}

func TestPortfolioManualLimitsUseTheStrictestStrategy(t *testing.T) {
	identity := `{"strategies":[{"id":1,"risk":{"max_gross_leverage":1.5,"max_asset_weight":1.5}},` +
		`{"id":2,"risk":{"max_gross_leverage":1.0,"max_asset_weight":0.4}}]}` + "\nmarket_data_mode=canonical-sqlite-v1"
	limits, ok := portfolioManualLimits(identity)
	if !ok || limits.grossPct != 100 || limits.assetPct != 40 {
		t.Fatalf("limits = %+v ok=%v", limits, ok)
	}
	if _, ok := portfolioManualLimits(`{"strategies":[{"id":1,"risk":{}}]}`); ok {
		t.Fatal("missing limits must disable manual control")
	}
}

func TestDailyCycleWindowMatchesExecutionState(t *testing.T) {
	at := func(hour, minute int) time.Time { return time.Date(2026, 10, 14, hour, minute, 0, 0, time.UTC) }
	for _, c := range []struct {
		time   time.Time
		inside bool
	}{{at(23, 44), false}, {at(23, 45), true}, {at(0, 0), true}, {at(0, 14), true}, {at(0, 15), false}, {at(12, 0), false}} {
		if inDailyCycleWindow(c.time) != c.inside {
			t.Fatalf("%s: inside = %v", c.time.Format("15:04"), !c.inside)
		}
	}
}
