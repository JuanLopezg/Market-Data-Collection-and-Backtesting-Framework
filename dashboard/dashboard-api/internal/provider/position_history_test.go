package provider

import (
	"bytes"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func near(a, b float64) bool { return math.Abs(a-b) < 1e-9 }

func TestPositionHistoryAveragesReducesAndFlips(t *testing.T) {
	events := []tradeEvent{
		{Time: 202001011, Sequence: 1, Kind: tradeFill, Asset: "BTC", Buy: true, Quantity: 1, Price: 100, Fee: 1},
		{Time: 202001021, Sequence: 2, Kind: tradeFill, Asset: "BTC", Buy: true, Quantity: 1, Price: 200, Fee: 2}, // avg 150
		{Time: 202001031, Kind: tradeFunding, Asset: "BTC", Amount: -0.5},
		{Time: 202001041, Sequence: 3, Kind: tradeFill, Asset: "BTC", Buy: false, Quantity: 0.5, Price: 210, Fee: 0.5}, // +30
		// Sells 2 with 1.5 held: closes at 120 (-45) and opens a 0.5 short with the rest.
		{Time: 202001051, Sequence: 4, Kind: tradeFill, Asset: "BTC", Buy: false, Quantity: 2, Price: 120, Fee: 4},
		{Time: 202001061, Kind: tradeFunding, Asset: "BTC", Amount: 0.25},
	}
	closed, open := buildPositionHistory(events)
	if len(closed) != 1 {
		t.Fatalf("closed: %+v", closed)
	}
	long := closed[0]
	// Fees: 1 + 2 + 0.5 + 4 x 1.5/2 = 6.5. Gross: 30 - 45 = -15. Net: -15 - 6.5 - 0.5 = -22.
	if long.Side != "Long" || !near(long.EntryPrice, 150) || !near(long.GrossPnL, -15) || !near(long.Fees, 6.5) ||
		!near(long.Funding, -0.5) || !near(long.NetPnL(), -22) || !near(long.MaxQuantity, 2) || long.ClosedAt != 202001051 {
		t.Fatalf("long: %+v net=%v", long, long.NetPnL())
	}
	if !near(long.EntryValue, 300) || !near(long.ExitValue, 0.5*210+1.5*120) || !near(long.ExitPrice(), 285.0/2) {
		t.Fatalf("long values: %+v", long)
	}
	short := open["BTC"]
	if short == nil || short.Side != "Short" || !near(short.Quantity, -0.5) || !near(short.Fees, 1) || !near(short.Funding, 0.25) || short.OpenedAt != 202001051 {
		t.Fatalf("short: %+v", short)
	}
}

func TestClosedPositionsCSVSplitsByCloseYear(t *testing.T) {
	events := []tradeEvent{
		{Time: 20231230, Sequence: 1, Kind: tradeFill, Asset: "ETH", Buy: true, Quantity: 2, Price: 1000, Fee: 1},
		{Time: 20240102, Sequence: 2, Kind: tradeFill, Asset: "ETH", Buy: false, Quantity: 2, Price: 1100, Fee: 1.1},
		{Time: 20240201, Sequence: 3, Kind: tradeFill, Asset: "SOL", Buy: true, Quantity: 10, Price: 50},
		{Time: 20240301, Sequence: 4, Kind: tradeFill, Asset: "SOL", Buy: false, Quantity: 10, Price: 40},
	}
	closed, _ := buildPositionHistory(events)
	report := ClosedPositionsReport{Closed: closed, Complete: true}
	dir := t.TempDir()
	files, err := WriteClosedPositionsReports(dir, report)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(files, ",") != "closed_positions.csv,closed_positions_2024.csv" {
		t.Fatalf("files: %v", files)
	}
	raw, err := os.ReadFile(filepath.Join(dir, "closed_positions_2024.csv"))
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(string(raw)), "\n")
	if len(lines) != 3 || lines[0] != strings.Join(closedPositionsCSVHeader, ",") {
		t.Fatalf("csv:\n%s", raw)
	}
	// ETH opened in 2023 and closed in 2024: it belongs to 2024. Net = 200 - 2.1.
	if lines[1] != "ETH,Long,2023-12-30,2024-01-02,2,1000,1100,2000.00000000,2200.00000000,200.00000000,2.10000000,0.00000000,197.90000000" {
		t.Fatalf("eth row: %s", lines[1])
	}

	if _, err := WriteClosedPositionsReports(t.TempDir(), ClosedPositionsReport{Closed: closed, Complete: false}); err == nil {
		t.Fatal("an incomplete history must not be written")
	}

	var all bytes.Buffer
	if err := WriteClosedPositionsCSV(&all, report, 0); err != nil || strings.Count(all.String(), "\n") != 3 {
		t.Fatalf("all years: %v\n%s", err, all.String())
	}
}

func TestTradeTimeLabel(t *testing.T) {
	for value, want := range map[uint64]string{202001011: "2020-01-01 open", 202001015: "2020-01-01 intraday", 202001019: "2020-01-01 close", 20240102: "2024-01-02"} {
		if got := tradeTimeLabel(value); got != want {
			t.Fatalf("%d: %q want %q", value, got, want)
		}
	}
}
