package provider

import (
	"fmt"
	"sort"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

type riskEvaluationView struct {
	StrategyID               uint64                    `json:"strategyId"`
	Name                     string                    `json:"name"`
	State                    string                    `json:"state"`
	Sizer                    string                    `json:"sizer"`
	Capital                  string                    `json:"capital"`
	GrossLimit               string                    `json:"grossLimit"`
	AssetLimit               string                    `json:"assetLimit"`
	GrossAfterAssetCap       string                    `json:"grossAfterAssetCap"`
	GrossScale               string                    `json:"grossScale"`
	VolatilityState          string                    `json:"volatilityState"`
	VolatilityTarget         string                    `json:"volatilityTarget"`
	RawSignalVolatility      string                    `json:"rawSignalVolatility"`
	VolatilityScale          string                    `json:"volatilityScale"`
	PreConstraintVolatility  string                    `json:"preConstraintVolatility"`
	PostConstraintVolatility string                    `json:"postConstraintVolatility"`
	Assets                   []riskAssetEvaluationView `json:"assets"`
}

type riskAssetEvaluationView struct {
	Asset             string `json:"asset"`
	SizedWeight       string `json:"sizedWeight"`
	AssetCappedWeight string `json:"assetCappedWeight"`
	ApprovedWeight    string `json:"approvedWeight"`
	CurrentQuantity   string `json:"currentQuantity"`
	Action            string `json:"action"`
	Reduction         string `json:"reduction"`
}

func applyRiskEvaluations(data *realRiskData, report *pgstore.RiskDiagnostics) {
	if report == nil {
		return
	}
	data.ConfigurationFingerprint = report.ConfigurationFingerprint
	data.SourceNote = "Approved instructions and same-cycle risk evaluations are persisted together. Evaluated weights are fractions of each strategy's capital. HOLD preserves existing quantity even when the newly evaluated weight differs; it does not emit a new target."
	data.MissingDiagnostics = []string{"Current marked portfolio weights and PnL", "Kill-switch/control readiness and whole-account/venue margin limits"}
	for _, evaluated := range report.Strategies {
		view := riskEvaluationView{StrategyID: evaluated.StrategyID, Name: evaluated.Name, Sizer: evaluated.Sizer,
			Capital: formatUSD(evaluated.ReferenceCapital), State: "EVALUATED",
			GrossLimit: formatPercent(evaluated.MaxGrossLeverage * 100), AssetLimit: formatPercent(evaluated.MaxAssetWeight * 100),
			GrossAfterAssetCap: formatPercent(evaluated.GrossAfterAssetCap * 100), GrossScale: fmt.Sprintf("%.6fx", evaluated.GrossScale),
			VolatilityState: evaluated.VolatilityState, VolatilityTarget: "Not applicable", RawSignalVolatility: "Not applicable",
			VolatilityScale: "Not applicable", PreConstraintVolatility: "Not applicable", PostConstraintVolatility: "Not applicable", Assets: []riskAssetEvaluationView{}}
		if !*evaluated.SizingAvailable {
			view.State = "SIZING UNAVAILABLE / HOLD"
			view.GrossAfterAssetCap, view.GrossScale = "Not evaluated", "Not evaluated"
			data.MissingDiagnostics = append(data.MissingDiagnostics, fmt.Sprintf("Strategy %d sizing unavailable; constraints were not evaluated", evaluated.StrategyID))
		}
		if evaluated.VolatilityState != "NOT_APPLICABLE" && evaluated.Volatility == nil {
			view.VolatilityTarget, view.RawSignalVolatility, view.VolatilityScale, view.PreConstraintVolatility, view.PostConstraintVolatility = "Not measured", "Not measured", "Not measured", "Not measured", "Not measured"
		}
		if volatility := evaluated.Volatility; volatility != nil {
			view.VolatilityTarget, view.RawSignalVolatility = formatPercent(volatility.Target*100), formatPercent(volatility.RawSignal*100)
			view.VolatilityScale = fmt.Sprintf("%.6fx", volatility.Scale)
			view.PreConstraintVolatility, view.PostConstraintVolatility = formatPercent(volatility.BeforeConstraints*100), formatPercent(volatility.AfterConstraints*100)
		}
		assets := make([]string, 0, len(evaluated.Assets))
		for asset := range evaluated.Assets {
			assets = append(assets, asset)
		}
		sort.Strings(assets)
		for _, asset := range assets {
			row := evaluated.Assets[asset]
			reason := "No cap reduction"
			if len(row.Reductions) > 0 {
				reason = strings.Join(row.Reductions, " + ")
			}
			if row.Action == "HOLD" {
				reason += "; rebalance policy preserves held quantity"
			}
			view.Assets = append(view.Assets, riskAssetEvaluationView{Asset: asset, SizedWeight: formatSignedPercent(*row.SizedWeight * 100), AssetCappedWeight: formatSignedPercent(*row.AssetCappedWeight * 100), ApprovedWeight: formatSignedPercent(*row.ApprovedWeight * 100), CurrentQuantity: fmt.Sprintf("%.8g", *row.CurrentQuantity), Action: row.Action, Reduction: reason})
		}
		data.RiskEvaluations = append(data.RiskEvaluations, view)
	}
}
