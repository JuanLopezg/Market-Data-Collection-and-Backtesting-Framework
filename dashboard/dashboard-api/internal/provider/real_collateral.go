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

// addKrakenCollateral adds the collateral, sizing capital, margin and net deposit cards.
func (p *Real) addKrakenCollateral(overview *realOverviewData, positions realPositionsData, days []pgstore.EquityDay) {
	if !p.krakenLive() {
		return
	}
	account, accountErr := p.readKrakenAccount(time.Now().UTC())
	collateral, known := latestCollateral(account, accountErr, days)
	if known {
		overview.Stats = append(overview.Stats, realOverviewMetric{Label: "BTC Collateral", Value: formatUSD(collateral),
			Detail: "After Kraken's haircut; margin only, never spent by orders"})
		if positions.ValuationState == "AVAILABLE" {
			overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Sizing Capital",
				Value: formatUSD(positions.equityValue + collateral), Detail: "Equity + BTC collateral after haircut"})
		}
	}
	switch {
	case accountErr != nil:
		overview.SourceWarnings = append(overview.SourceWarnings, "Kraken account monitor unavailable: "+accountErr.Error())
	case !account.fresh:
		overview.SourceWarnings = append(overview.SourceWarnings,
			"Kraken account monitor is stale (last observation "+account.at.Format("2006-01-02 15:04 UTC")+")")
	default:
		if headroom, ok := decimalText(account.file.MaintenanceHeadroomUSD); ok {
			overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Margin Headroom", Value: formatUSD(headroom),
				Detail: "Kraken margin equity minus maintenance margin"})
		}
		overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Drop to Maintenance",
			Value:  "BTC " + dropLabel(account.file.BTCDrop) + " · all " + dropLabel(account.file.CryptoDrop),
			Detail: "Price fall that would use up the headroom: BTC collateral alone, and BTC with the net long positions"})
	}
	if len(days) > 0 && days[len(days)-1].CapitalFlows != 0 {
		overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Net Deposits",
			Value: formatUSD(days[len(days)-1].CapitalFlows), Detail: "USD deposited minus withdrawn; not performance"})
	}
}
