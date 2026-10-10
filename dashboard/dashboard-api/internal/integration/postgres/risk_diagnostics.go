package postgres

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
)

// Read-only evaluation evidence persisted atomically with the owning risk decision.
// It is not an order/control contract and is never substituted for DecisionBatch.
type RiskDiagnostics struct {
	SchemaVersion            int                      `json:"schema_version"`
	Timestamp                uint64                   `json:"timestamp"`
	ConfigurationIdentity    string                   `json:"configuration_identity"`
	ConfigurationFingerprint string                   `json:"-"`
	Strategies               []RiskStrategyEvaluation `json:"strategies"`
}

type RiskStrategyEvaluation struct {
	StrategyID         uint64                         `json:"strategy_id"`
	Name               string                         `json:"name"`
	Sizer              string                         `json:"sizer"`
	ReferenceCapital   float64                        `json:"reference_capital"`
	SizingAvailable    *bool                          `json:"sizing_available"`
	MaxGrossLeverage   float64                        `json:"max_gross_leverage"`
	MaxAssetWeight     float64                        `json:"max_asset_weight"`
	GrossAfterAssetCap float64                        `json:"gross_after_asset_cap"`
	GrossScale         float64                        `json:"gross_scale"`
	VolatilityState    string                         `json:"volatility_state"`
	Volatility         *RiskVolatilityEvaluation      `json:"volatility"`
	Assets             map[string]RiskAssetEvaluation `json:"assets"`
}

type RiskVolatilityEvaluation struct {
	Target            float64 `json:"target"`
	RawSignal         float64 `json:"raw_signal"`
	Scale             float64 `json:"scale"`
	BeforeConstraints float64 `json:"before_constraints"`
	AfterConstraints  float64 `json:"after_constraints"`
}

type RiskAssetEvaluation struct {
	SizedWeight       *float64 `json:"sized_weight"`
	AssetCappedWeight *float64 `json:"asset_capped_weight"`
	ApprovedWeight    *float64 `json:"approved_weight"`
	CurrentQuantity   *float64 `json:"current_quantity"`
	Action            string   `json:"action"`
	Reductions        []string `json:"reductions"`
}

func decodeRiskDiagnostics(payload string, timestamp uint64, identity string) (*RiskDiagnostics, error) {
	var report RiskDiagnostics
	if len(payload) > 1024*1024 || json.Unmarshal([]byte(payload), &report) != nil ||
		report.SchemaVersion != 1 || report.Timestamp != timestamp || report.ConfigurationIdentity == "" ||
		(identity != "" && report.ConfigurationIdentity != identity) || len(report.Strategies) == 0 || len(report.Strategies) > 128 {
		return nil, fmt.Errorf("risk diagnostics schema, configuration identity or cycle anchor invalid")
	}
	ids := map[uint64]bool{}
	var document struct {
		Strategies []map[string]json.RawMessage `json:"strategies"`
	}
	json.Unmarshal([]byte(payload), &document)
	for index, strategy := range report.Strategies {
		for _, field := range []string{"reference_capital", "max_gross_leverage", "max_asset_weight", "gross_after_asset_cap", "gross_scale"} {
			if value := document.Strategies[index][field]; len(value) == 0 || string(value) == "null" {
				return nil, fmt.Errorf("risk diagnostics missing numeric evaluation")
			}
		}
		if strategy.StrategyID == 0 || ids[strategy.StrategyID] || strategy.Name == "" || strategy.SizingAvailable == nil ||
			strategy.ReferenceCapital < 0 || strategy.MaxGrossLeverage < 0 || strategy.MaxAssetWeight < 0 ||
			strategy.GrossAfterAssetCap < 0 || strategy.GrossScale < 0 || strategy.GrossScale > 1 || len(strategy.Assets) > 10000 {
			return nil, fmt.Errorf("risk diagnostics strategy evaluation invalid")
		}
		ids[strategy.StrategyID] = true
		switch strategy.VolatilityState {
		case "OBSERVED", "NOT_APPLICABLE", "NO_ACTIVE_SIGNALS", "UNAVAILABLE", "DIAGNOSTIC_FAILED":
		default:
			return nil, fmt.Errorf("risk diagnostics volatility state invalid")
		}
		if !*strategy.SizingAvailable && len(strategy.Assets) != 0 {
			return nil, fmt.Errorf("unavailable sizing contains evaluated asset weights")
		}
		if (strategy.VolatilityState == "OBSERVED") != (strategy.Volatility != nil) {
			return nil, fmt.Errorf("risk diagnostics volatility availability invalid")
		}
		if strategy.Volatility != nil {
			var values map[string]*float64
			if json.Unmarshal(document.Strategies[index]["volatility"], &values) != nil {
				return nil, fmt.Errorf("risk diagnostics volatility evaluation invalid")
			}
			for _, field := range []string{"target", "raw_signal", "scale", "before_constraints", "after_constraints"} {
				if value := values[field]; value == nil || *value < 0 {
					return nil, fmt.Errorf("risk diagnostics missing or invalid volatility value")
				}
			}
		}
		for asset, row := range strategy.Assets {
			if asset == "" || row.SizedWeight == nil || row.AssetCappedWeight == nil || row.ApprovedWeight == nil || row.CurrentQuantity == nil ||
				(row.Action != "HOLD" && row.Action != "FLAT" && row.Action != "TARGET_WEIGHT") {
				return nil, fmt.Errorf("risk diagnostics asset evaluation invalid")
			}
			for _, reason := range row.Reductions {
				if reason != "ASSET_CAP" && reason != "GROSS_CAP" {
					return nil, fmt.Errorf("risk diagnostics reduction reason invalid")
				}
			}
		}
	}
	digest := sha256.Sum256([]byte(report.ConfigurationIdentity))
	report.ConfigurationFingerprint = hex.EncodeToString(digest[:])
	return &report, nil
}
