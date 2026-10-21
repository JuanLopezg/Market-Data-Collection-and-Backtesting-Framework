package provider

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	krakenpublic "control-dashboard-api/internal/integration/kraken"
	"control-dashboard-api/internal/integration/symbolmap"
)

func writeSymbolMap(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "symbol_map.json")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

const testSymbolMapJSON = `{"schema_version":1,"venue":"kraken-futures","generated_at":"2026-10-13T17:31:09Z","symbols":[
 {"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1},
 {"source":"1000PEPEUSDT","asset":"PEPE","venue_symbol":"PF_PEPEUSD","venue_units_per_source_unit":1000},
 {"source":"ETHUSDT","asset":"ETH","venue_symbol":"PF_ETHUSD","venue_units_per_source_unit":1},
 {"source":"OLDUSDT","asset":"OLD","venue_symbol":"PF_OLDUSD","venue_units_per_source_unit":1},
 {"source":"HALTUSDT","asset":"HALT","venue_symbol":"PF_HALTUSD","venue_units_per_source_unit":1}]}`

func testSymbolMapInput(t *testing.T) symbolMapInput {
	t.Helper()
	table, err := symbolmap.Load(writeSymbolMap(t, testSymbolMapJSON), "kraken-futures")
	if err != nil {
		t.Fatal(err)
	}
	return symbolMapInput{
		table:         table,
		strategy:      []string{"BTCUSDT", "1000PEPEUSDT", "ARCUSDT", "WUSDT", "OLDUSDT", "HALTUSDT"},
		activeSignals: map[string]bool{"WUSDT": true},
		ranking:       []string{"BTCUSDT", "ETHUSDT", "CTUSDT"},
		closes: map[string]float64{"BTCUSDT": 62000, "1000PEPEUSDT": 0.0102, "ETHUSDT": 2500,
			"ARCUSDT": 0.5, "WUSDT": 0.1, "OLDUSDT": 1, "HALTUSDT": 3, "CTUSDT": 0.2},
		tickers: map[string]krakenpublic.Ticker{
			"PF_XBTUSD":  {Symbol: "PF_XBTUSD", MarkPrice: 61800},
			"PF_PEPEUSD": {Symbol: "PF_PEPEUSD", MarkPrice: 0.0000101},
			"PF_ETHUSD":  {Symbol: "PF_ETHUSD", MarkPrice: 2510},
			"PF_HALTUSD": {Symbol: "PF_HALTUSD", MarkPrice: 3, Suspended: true},
		},
	}
}

func rowsBySource(status SymbolMapStatus) map[string]SymbolMapRow {
	rows := map[string]SymbolMapRow{}
	for _, row := range status.Rows {
		rows[row.Source] = row
	}
	return rows
}

func TestSymbolMapCheckStates(t *testing.T) {
	status := buildSymbolMapStatus(testSymbolMapInput(t), time.Date(2026, 10, 13, 18, 0, 0, 0, time.UTC))
	rows := rowsBySource(status)

	want := map[string]string{
		"BTCUSDT": "OK", "1000PEPEUSDT": "OK", "ETHUSDT": "OK",
		"ARCUSDT": "NOT_ON_VENUE", "WUSDT": "NOT_ON_VENUE", "CTUSDT": "NOT_ON_VENUE",
		"OLDUSDT": "NOT_LISTED", "HALTUSDT": "SUSPENDED",
	}
	for source, state := range want {
		if rows[source].State != state {
			t.Errorf("%s state = %q, want %q (%s)", source, rows[source].State, state, rows[source].Detail)
		}
	}
	if pepe := rows["1000PEPEUSDT"]; pepe.Scale != 1000 || pepe.PriceRatio < 999 || pepe.PriceRatio > 1011 ||
		!strings.Contains(pepe.Detail, "1 1000PEPEUSDT = 1000 PEPE on PF_PEPEUSD") {
		t.Errorf("unexpected 1000PEPEUSDT row: %+v", pepe)
	}
	if status.StrategyAssets != 6 || status.StrategyMapped != 2 {
		t.Errorf("strategy counts = %d/%d, want 2/6", status.StrategyMapped, status.StrategyAssets)
	}
	if !status.Rows[0].InStrategy || status.Rows[len(status.Rows)-1].InStrategy {
		t.Errorf("strategy assets must come first: %+v", status.Rows)
	}
	// W has an active signal it cannot trade: an ALERT, but only its orders are skipped, so WARN.
	if status.Status != "WARN" {
		t.Errorf("status = %q, want WARN", status.Status)
	}
}

func TestSymbolMapAlarms(t *testing.T) {
	status := buildSymbolMapStatus(testSymbolMapInput(t), time.Now())
	severity := map[string]string{}
	for _, alarm := range status.Alarms {
		severity[alarm.Asset+" "+alarm.EventType] = alarm.Severity
	}
	want := map[string]string{
		"WUSDT SYMBOL_MAP_SIGNAL_NOT_TRADEABLE":      "ALERT",
		"ARCUSDT SYMBOL_MAP_STRATEGY_NOT_TRADEABLE":  "WARN",
		"OLDUSDT SYMBOL_MAP_STRATEGY_NOT_TRADEABLE":  "WARN",
		"HALTUSDT SYMBOL_MAP_STRATEGY_NOT_TRADEABLE": "WARN",
		"CTUSDT SYMBOL_MAP_RANKING_NOT_TRADEABLE":    "INFO",
	}
	if len(severity) != len(want) {
		t.Fatalf("alarms = %v, want %v", severity, want)
	}
	for key, value := range want {
		if severity[key] != value {
			t.Errorf("alarm %s = %q, want %q", key, severity[key], value)
		}
	}
}

func TestSymbolMapFindsAssetsMissingFromAnOutOfDateTable(t *testing.T) {
	in := testSymbolMapInput(t)
	// New top-50 entries after the table was built: Kraken lists SOL and SHIB (in single
	// SHIB) but the table does not; NEIRO exists on Kraken under the same name but is a
	// different token (price far off), so it is not proposed; CT is not on Kraken at all.
	in.strategy, in.activeSignals = nil, map[string]bool{}
	in.ranking = []string{"SOLUSDT", "1000SHIBUSDT", "NEIROUSDT", "CTUSDT"}
	in.closes = map[string]float64{"SOLUSDT": 180, "1000SHIBUSDT": 0.0121, "NEIROUSDT": 0.0004, "CTUSDT": 0.2}
	in.tickers["PF_SOLUSD"] = krakenpublic.Ticker{Symbol: "PF_SOLUSD", MarkPrice: 179.5}
	in.tickers["PF_SHIBUSD"] = krakenpublic.Ticker{Symbol: "PF_SHIBUSD", MarkPrice: 0.0000120}
	in.tickers["PF_NEIROUSD"] = krakenpublic.Ticker{Symbol: "PF_NEIROUSD", MarkPrice: 0.09}

	status := buildSymbolMapStatus(in, time.Now())
	rows := rowsBySource(status)
	if row := rows["SOLUSDT"]; row.State != "MISSING_FROM_TABLE" || row.VenueSymbol != "PF_SOLUSD" || row.Scale != 1 {
		t.Errorf("SOLUSDT: %+v", row)
	}
	if row := rows["1000SHIBUSDT"]; row.State != "MISSING_FROM_TABLE" || row.VenueSymbol != "PF_SHIBUSD" || row.Scale != 1000 {
		t.Errorf("1000SHIBUSDT: %+v", row)
	}
	if rows["NEIROUSDT"].State != "NOT_ON_VENUE" || rows["CTUSDT"].State != "NOT_ON_VENUE" {
		t.Errorf("NEIRO/CT must not be proposed: %+v %+v", rows["NEIROUSDT"], rows["CTUSDT"])
	}

	severity := map[string]string{}
	for _, alarm := range status.Alarms {
		severity[alarm.Asset] = alarm.Severity + " " + alarm.EventType
	}
	if severity["SOLUSDT"] != "WARN SYMBOL_MAP_TABLE_OUT_OF_DATE" || severity["1000SHIBUSDT"] != "WARN SYMBOL_MAP_TABLE_OUT_OF_DATE" ||
		severity["CTUSDT"] != "INFO SYMBOL_MAP_RANKING_NOT_TRADEABLE" || severity["NEIROUSDT"] != "INFO SYMBOL_MAP_RANKING_NOT_TRADEABLE" {
		t.Errorf("unexpected alarms: %v", severity)
	}
	if status.Status != "WARN" {
		t.Errorf("status = %q, want WARN (out-of-date table)", status.Status)
	}

	// Only top-50 assets Kraken does not list: informational, the check stays OK.
	in.ranking = []string{"CTUSDT"}
	if status := buildSymbolMapStatus(in, time.Now()); status.Status != "OK" || len(status.Alarms) != 1 {
		t.Errorf("INFO alarms must not change the status: %+v", status)
	}
}

func TestSymbolMapToleratesALargeMoveSinceTheClose(t *testing.T) {
	// ARC on 2026-10-13: yesterday's Binance close 0.0595, Kraken mark 0.0330 after a ~45%
	// fall. Same token and scale; this must not be a wrong-scale alarm.
	in := testSymbolMapInput(t)
	in.strategy, in.ranking = nil, []string{"BTCUSDT"}
	in.closes["BTCUSDT"] = 0.0595
	in.tickers["PF_XBTUSD"] = krakenpublic.Ticker{Symbol: "PF_XBTUSD", MarkPrice: 0.0330}
	status := buildSymbolMapStatus(in, time.Now())
	if row := rowsBySource(status)["BTCUSDT"]; row.State != "OK" || len(status.Alarms) != 0 {
		t.Fatalf("a 1.8x price move is not a wrong scale: %+v %+v", row, status.Alarms)
	}
}

func TestSymbolMapWrongScaleIsCritical(t *testing.T) {
	in := testSymbolMapInput(t)
	// A table that forgot the x1000: the Binance/Kraken ratio (~1000) disagrees with x1.
	wrong := strings.Replace(testSymbolMapJSON, `"PF_PEPEUSD","venue_units_per_source_unit":1000`, `"PF_PEPEUSD","venue_units_per_source_unit":1`, 1)
	table, err := symbolmap.Load(writeSymbolMap(t, wrong), "kraken-futures")
	if err != nil {
		t.Fatal(err)
	}
	in.table = table
	in.strategy, in.ranking = nil, []string{"1000PEPEUSDT"} // even outside the strategy
	status := buildSymbolMapStatus(in, time.Now())
	if status.Status != "BLOCKED" || len(status.Alarms) != 1 || status.Alarms[0].EventType != "SYMBOL_MAP_SCALE_MISMATCH" {
		t.Fatalf("wrong scale must be critical: %+v", status)
	}
	if rows := rowsBySource(status); rows["1000PEPEUSDT"].State != "SCALE_MISMATCH" {
		t.Fatalf("row state = %q", rows["1000PEPEUSDT"].State)
	}
}

func TestSymbolMapLoadRejectsInvalidTables(t *testing.T) {
	cases := map[string]string{
		"other venue": `{"schema_version":1,"venue":"hyperliquid","symbols":[]}`,
		"duplicate source": `{"schema_version":1,"venue":"kraken-futures","symbols":[
			{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1},
			{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_BTCUSD","venue_units_per_source_unit":1}]}`,
		"duplicate venue symbol": `{"schema_version":1,"venue":"kraken-futures","symbols":[
			{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1},
			{"source":"XBTUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1}]}`,
		"zero scale": `{"schema_version":1,"venue":"kraken-futures","symbols":[
			{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":0}]}`,
		"missing asset": `{"schema_version":1,"venue":"kraken-futures","symbols":[
			{"source":"BTCUSDT","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1}]}`,
	}
	for name, body := range cases {
		if _, err := symbolmap.Load(writeSymbolMap(t, body), "kraken-futures"); err == nil {
			t.Errorf("%s: expected an error", name)
		}
	}
}

func TestRepositorySymbolMapLoads(t *testing.T) {
	path := filepath.Join("..", "..", "..", "..", "config", "venues", "kraken_futures", "symbol_map.json")
	table, err := symbolmap.Load(path, "kraken-futures")
	if err != nil {
		t.Skipf("repository symbol map not reachable from this test: %v", err)
	}
	pepe, ok := table.BySource("1000PEPEUSDT")
	if !ok || pepe.VenueSymbol != "PF_PEPEUSD" || pepe.VenueUnitsPerSourceUnit != 1000 {
		t.Fatalf("unexpected 1000PEPEUSDT entry: %+v", pepe)
	}
}

func TestKrakenPublicTickers(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			t.Errorf("method = %s, want GET", r.Method)
		}
		_, _ = w.Write([]byte(`{"result":"success","tickers":[
			{"symbol":"PF_XBTUSD","markPrice":61800.5,"suspended":false},
			{"symbol":"PF_HALTUSD","markPrice":3,"suspended":true},
			{"symbol":"FI_XBTUSD_261225","markPrice":62000}]}`))
	}))
	defer server.Close()

	tickers, err := krakenpublic.NewPublicClient(server.URL, time.Second).Tickers(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if len(tickers) != 2 || tickers["PF_XBTUSD"].MarkPrice != 61800.5 || !tickers["PF_HALTUSD"].Suspended {
		t.Fatalf("unexpected tickers: %+v", tickers)
	}
	if _, err := krakenpublic.NewPublicClient("", time.Second).Tickers(context.Background()); err == nil {
		t.Fatal("an unconfigured client must fail")
	}
}

func TestMergeSymbolMapAlerts(t *testing.T) {
	status := buildSymbolMapStatus(testSymbolMapInput(t), time.Date(2026, 10, 13, 18, 0, 0, 0, time.UTC))
	got := mergeSymbolMapAlerts(realAlertsAuditData{}, status)
	if got.ActiveCritical != 0 || got.ActiveAlerts != 1 || got.ActiveWarnings != 3 {
		t.Fatalf("critical=%d alerts=%d warnings=%d, want 0, 1 and 3", got.ActiveCritical, got.ActiveAlerts, got.ActiveWarnings)
	}
	if got.Alerts[0].ID != "derived-symbol-map-symbol-map-signal-not-tradeable-wusdt" || got.Alerts[0].Service != "SymbolMap" {
		t.Fatalf("unexpected first alert: %+v", got.Alerts[0])
	}

	unavailable := mergeSymbolMapAlerts(realAlertsAuditData{}, unavailableSymbolMapStatus(os.ErrNotExist, time.Now()))
	if unavailable.ActiveCritical != 0 || unavailable.ActiveWarnings != 1 {
		t.Fatalf("an unreadable table or Kraken outage is a warning: %+v", unavailable)
	}
}
