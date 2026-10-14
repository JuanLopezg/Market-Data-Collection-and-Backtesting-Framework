package provider

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

func writeKrakenAccount(t *testing.T, observedAt time.Time) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "account.json")
	body := `{"version":"kraken-account-v1","observedAt":"` + observedAt.Format(time.RFC3339Nano) + `",
		"btc_quantity":"0.1","btc_value_usd":"5000","btc_collateral_usd":"4950","maintenance_margin_usd":"250",
		"maintenance_headroom_usd":"990","btc_drop_to_maintenance":"0.2","crypto_drop_to_maintenance":"NONE"}`
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func statValue(stats []realOverviewMetric, label string) string {
	for _, stat := range stats {
		if stat.Label == label {
			return stat.Value
		}
	}
	return ""
}

func TestKrakenCollateralCardsOnlyInLive(t *testing.T) {
	now := time.Now().UTC()
	days := []pgstore.EquityDay{{Date: 20261013, Equity: 1000, CollateralUSD: 4900, CapitalFlows: 250}}
	positions := realPositionsData{ValuationState: "AVAILABLE", equityValue: 1000}

	live := &Real{cfg: RealConfig{RuntimeMode: "LIVE", ExecutionVenue: "KRAKEN", KrakenAccountFile: writeKrakenAccount(t, now)}}
	overview := realOverviewData{}
	live.addKrakenCollateral(&overview, positions, days)
	// The fresh monitor value wins over the daily one.
	if got := statValue(overview.Stats, "BTC Collateral"); got != formatUSD(4950) {
		t.Fatalf("BTC Collateral = %q", got)
	}
	if got := statValue(overview.Stats, "Sizing Capital"); got != formatUSD(5950) {
		t.Fatalf("Sizing Capital = %q", got)
	}
	if got := statValue(overview.Stats, "Drop to Maintenance"); got != "BTC 20.0% · all no long exposure" {
		t.Fatalf("Drop to Maintenance = %q", got)
	}
	if got := statValue(overview.Stats, "Net Deposits"); got != formatUSD(250) {
		t.Fatalf("Net Deposits = %q", got)
	}

	// Stale monitor: the collateral of the latest sized day, no margin cards, a warning.
	stale := &Real{cfg: RealConfig{RuntimeMode: "LIVE", ExecutionVenue: "KRAKEN",
		KrakenAccountFile: writeKrakenAccount(t, now.Add(-10*time.Minute))}}
	overview = realOverviewData{}
	stale.addKrakenCollateral(&overview, positions, days)
	if statValue(overview.Stats, "BTC Collateral") != formatUSD(4900) || statValue(overview.Stats, "Margin Headroom") != "" {
		t.Fatalf("stale monitor stats = %+v", overview.Stats)
	}
	if len(overview.SourceWarnings) != 1 || !strings.Contains(overview.SourceWarnings[0], "stale") {
		t.Fatalf("stale warning = %v", overview.SourceWarnings)
	}

	// PAPER: the real Kraken account has nothing to do with the simulated one.
	paper := &Real{cfg: RealConfig{RuntimeMode: "PAPER", KrakenAccountFile: writeKrakenAccount(t, now)}}
	overview = realOverviewData{}
	paper.addKrakenCollateral(&overview, positions, days)
	if len(overview.Stats) != 0 || len(overview.SourceWarnings) != 0 {
		t.Fatalf("PAPER got Kraken cards: %+v", overview)
	}
}

func TestDropLabel(t *testing.T) {
	half, more, none, bad := "0.5", "1.3", "NONE", "x"
	for value, want := range map[*string]string{nil: "unavailable", &half: "50.0%", &more: ">100%", &none: "no long exposure", &bad: "unavailable"} {
		if got := dropLabel(value); got != want {
			t.Fatalf("dropLabel = %q, want %q", got, want)
		}
	}
}
