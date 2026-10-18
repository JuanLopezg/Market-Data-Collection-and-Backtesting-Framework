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
		"usdBalance":"1000","btc_quantity":"0.1","btc_value_usd":"5000","btc_collateral_usd":"4950","maintenance_margin_usd":"250",
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
	// LIVE valuation: 1,000 USD of cash and positions plus 4,950 USD of BTC collateral.
	positions := realPositionsData{ValuationState: "AVAILABLE", equityValue: 5950, collateralValue: 4950}

	live := &Real{cfg: RealConfig{RuntimeMode: "LIVE", ExecutionVenue: "KRAKEN", KrakenAccountFile: writeKrakenAccount(t, now)}}
	overview := realOverviewData{Stats: []realOverviewMetric{{Label: "Total Equity", Value: "Not available"}, {Label: "Cash", Value: "$40.00"}},
		EquityCurve: []realEquityPoint{{Label: "2026-10-13", Equity: 1000}}}
	live.addKrakenCollateral(&overview, positions, days)
	// One Total Equity with the collateral inside, and its BTC share (fresh monitor value).
	if got := statValue(overview.Stats, "Total Equity"); got != formatUSD(5950) {
		t.Fatalf("Total Equity = %q", got)
	}
	// Cash becomes Collateral: USD balance + BTC after haircut, and the BTC share of it.
	if got := statValue(overview.Stats, "Collateral"); got != formatUSD(5950)+" · 83% BTC" {
		t.Fatalf("Collateral = %q", got)
	}
	if overview.Stats[1].Label != "Collateral" || statValue(overview.Stats, "Cash") != "" ||
		statValue(overview.Stats, "In BTC Collateral") != "" || statValue(overview.Stats, "Drop to Maintenance") != "" ||
		statValue(overview.Stats, "Margin Headroom") != "" {
		t.Fatalf("collateral cards = %+v", overview.Stats)
	}
	if overview.EquityCurve[0].Equity != 5900 {
		t.Fatalf("equity curve must include the day's collateral: %+v", overview.EquityCurve)
	}
	if got := statValue(overview.Stats, "Net Deposits"); got != formatUSD(250) {
		t.Fatalf("Net Deposits = %q", got)
	}

	// Stale monitor: the collateral of the latest sized day, no margin cards, a warning.
	stale := &Real{cfg: RealConfig{RuntimeMode: "LIVE", ExecutionVenue: "KRAKEN",
		KrakenAccountFile: writeKrakenAccount(t, now.Add(-10*time.Minute))}}
	overview = realOverviewData{}
	stale.addKrakenCollateral(&overview, realPositionsData{}, days)
	// No current valuation: the latest daily equity plus that day's collateral.
	if statValue(overview.Stats, "Total Equity") != formatUSD(5900) ||
		statValue(overview.Stats, "Collateral") != formatUSD(4900)+" BTC (USD balance unavailable)" ||
		statValue(overview.Stats, "Margin Headroom") != "" {
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

func TestOverviewShowsGrossAndNetExposure(t *testing.T) {
	overview := realOverviewData{Stats: []realOverviewMetric{{Label: "Total Equity"}, {Label: "Cash"}, {Label: "Leverage"}}}
	addPositionValuation(&overview, realPositionsData{ValuationState: "AVAILABLE", TotalEquity: "$226.83",
		GrossExposure: "$60.28", GrossExposurePct: "26.58%", NetExposure: "+$60.28", equityValue: 226.83, grossValue: 60.28})
	if statValue(overview.Stats, "Gross Exposure") != "$60.28" || statValue(overview.Stats, "Net Exposure") != "+$60.28" {
		t.Fatalf("exposure cards = %+v", overview.Stats)
	}
	// LIVE cards in the chosen order: Equity, Gross, Net, Collateral, Leverage; others after.
	overview.Stats[1].Label = "Collateral"
	overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Net Deposits"})
	orderOverviewStats(&overview)
	var labels []string
	for _, stat := range overview.Stats {
		labels = append(labels, stat.Label)
	}
	if got := strings.Join(labels, ","); got != "Total Equity,Gross Exposure,Net Exposure,Collateral,Leverage,Net Deposits" {
		t.Fatalf("card order = %s", got)
	}
}
