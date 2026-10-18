package provider

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

// Kraken LIVE only: BTC collateral, margin and capital flows (roadmap step 8).
//
// Sources:
//   - account.json from the read-only account monitor (host unit, every 30 s): BTC
//     collateral after haircut, maintenance margin and the drop that would reach it;
//   - ExecutionState's daily equity rows: the collateral each day was sized with and the
//     net USD deposited or withdrawn (capital flows are not performance).
//
// The strategy sizes with equity + BTC collateral after haircut (user decision 2026-10-14);
// the equity curve stays cash + positions.

// A monitor observation older than this is shown as stale and not used for sizing figures.
const krakenAccountMaxAge = 5 * time.Minute

type krakenAccountFile struct {
	ObservedAt             string  `json:"observedAt"`
	USDBalance             string  `json:"usdBalance"`
	BTCQuantity            string  `json:"btc_quantity"`
	BTCValueUSD            string  `json:"btc_value_usd"`
	BTCCollateralUSD       string  `json:"btc_collateral_usd"`
	MaintenanceMarginUSD   string  `json:"maintenance_margin_usd"`
	MaintenanceHeadroomUSD string  `json:"maintenance_headroom_usd"`
	BTCDrop                *string `json:"btc_drop_to_maintenance"`
	CryptoDrop             *string `json:"crypto_drop_to_maintenance"`
}

type krakenAccountView struct {
	file  krakenAccountFile
	at    time.Time
	fresh bool
}

func (p *Real) readKrakenAccount(now time.Time) (krakenAccountView, error) {
	path := strings.TrimSpace(p.cfg.KrakenAccountFile)
	if path == "" {
		return krakenAccountView{}, fmt.Errorf("Kraken account file not configured")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return krakenAccountView{}, err
	}
	var view krakenAccountView
	if err := json.Unmarshal(raw, &view.file); err != nil {
		return krakenAccountView{}, fmt.Errorf("decode Kraken account file: %w", err)
	}
	view.at, err = time.Parse(time.RFC3339Nano, view.file.ObservedAt)
	if err != nil {
		return krakenAccountView{}, fmt.Errorf("Kraken account file has no valid observedAt")
	}
	view.fresh = now.Sub(view.at) <= krakenAccountMaxAge
	return view, nil
}

func decimalText(value string) (float64, bool) {
	number, err := strconv.ParseFloat(strings.TrimSpace(value), 64)
	return number, err == nil && !math.IsNaN(number) && !math.IsInf(number, 0)
}

// collateralCard shows the Kraken collateral in USD, the USD balance plus the BTC after
// Kraken's haircut, and the share of it that is BTC, e.g. "$226.83 · 55% BTC". Without a
// fresh monitor reading only the BTC value of the latest sized day is known.
func collateralCard(account krakenAccountView, accountErr error, btcUSD float64) realOverviewMetric {
	card := realOverviewMetric{Label: "Collateral",
		Detail: "Kraken USD balance + BTC after haircut: margin, not positions; orders never sell the BTC"}
	usd, usdKnown := decimalText(account.file.USDBalance)
	if accountErr != nil || !account.fresh || !usdKnown || usd+btcUSD <= 0 {
		card.Value = formatUSD(btcUSD) + " BTC (USD balance unavailable)"
		return card
	}
	card.Value = fmt.Sprintf("%s · %.0f%% BTC", formatUSD(usd+btcUSD), btcUSD/(usd+btcUSD)*100)
	return card
}

// dropLabel turns the monitor's fraction ("0.35", "NONE" or null) into "35.0%".
func dropLabel(value *string) string {
	if value == nil {
		return "unavailable"
	}
	if *value == "NONE" {
		return "no long exposure"
	}
	fraction, ok := decimalText(*value)
	switch {
	case !ok:
		return "unavailable"
	case fraction >= 1:
		return ">100%"
	default:
		return fmt.Sprintf("%.1f%%", fraction*100)
	}
}

// latestCollateral is the BTC collateral after haircut to size with now: the monitor's fresh
// value, else the one ExecutionState used for the latest day.
func latestCollateral(account krakenAccountView, accountErr error, days []pgstore.EquityDay) (float64, bool) {
	if accountErr == nil && account.fresh {
		if value, ok := decimalText(account.file.BTCCollateralUSD); ok {
			return value, true
		}
	}
	if len(days) > 0 {
		return days[len(days)-1].CollateralUSD, true
	}
	return 0, false
}

// addKrakenCollateral makes Total Equity the whole Kraken capital (USD cash, positions at
// Kraken's mark and BTC collateral after haircut, as the risk service sizes it), shows the
// BTC share of it, and adds the net deposit card. The daily equity curve gets
// each day's collateral too, so curve and card measure the same thing.
func (p *Real) addKrakenCollateral(overview *realOverviewData, positions realPositionsData, days []pgstore.EquityDay) {
	if !p.krakenLive() {
		return
	}
	account, accountErr := p.readKrakenAccount(time.Now().UTC())
	collateral, known := latestCollateral(account, accountErr, days)
	if len(days) > 0 && len(overview.EquityCurve) == len(days) {
		for index, day := range days {
			overview.EquityCurve[index].Equity += day.CollateralUSD
		}
	}
	total, detail := 0.0, ""
	switch {
	case positions.ValuationState == "AVAILABLE":
		total = positions.equityValue // already includes the collateral
		detail = "USD balance + positions at Kraken mark + BTC collateral after haircut"
	case len(days) > 0:
		total = days[len(days)-1].Equity + days[len(days)-1].CollateralUSD
		detail = "At the latest daily close, BTC collateral after haircut included"
	}
	if total > 0 {
		equity := realOverviewMetric{Label: "Total Equity", Value: formatUSD(total), Detail: detail}
		if len(overview.Stats) == 0 {
			overview.Stats = append(overview.Stats, equity)
		} else {
			overview.Stats[0] = equity // the overview's first card
		}
	}
	if known {
		// The account's money, not its trades: USD and BTC are collateral, never positions.
		card := collateralCard(account, accountErr, collateral)
		if len(overview.Stats) > 1 && overview.Stats[1].Label == "Cash" {
			overview.Stats[1] = card
		} else {
			overview.Stats = append(overview.Stats, card)
		}
	}
	switch {
	case accountErr != nil:
		overview.SourceWarnings = append(overview.SourceWarnings, "Kraken account monitor unavailable: "+accountErr.Error())
	case !account.fresh:
		overview.SourceWarnings = append(overview.SourceWarnings,
			"Kraken account monitor is stale (last observation "+account.at.Format("2006-01-02 15:04 UTC")+")")
	}
	if len(days) > 0 && days[len(days)-1].CapitalFlows != 0 {
		overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Net Deposits",
			Value: formatUSD(days[len(days)-1].CapitalFlows), Detail: "USD deposited minus withdrawn; not performance"})
	}
}
