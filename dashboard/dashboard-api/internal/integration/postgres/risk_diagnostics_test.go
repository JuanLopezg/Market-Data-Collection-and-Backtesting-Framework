package postgres

import (
	"encoding/json"
	"testing"
)

func TestRiskDiagnosticsAnchorConfigurationAndMissingValues(t *testing.T) {
	identity := "fixture policy identity"
	strategy := map[string]any{"strategy_id": 1, "name": "Fixture", "sizer": "EqualWeight", "reference_capital": 980,
		"sizing_available": true, "max_gross_leverage": 0.4, "max_asset_weight": 0.25, "gross_after_asset_cap": 0.5,
		"gross_scale": 0.8, "volatility_state": "NOT_APPLICABLE", "assets": map[string]any{
			"ETH": map[string]any{"sized_weight": -0.5, "asset_capped_weight": -0.25, "approved_weight": -0.2, "current_quantity": -2, "action": "HOLD", "reductions": []string{"ASSET_CAP", "GROSS_CAP"}}}}
	report := map[string]any{"schema_version": 1, "timestamp": 20261007, "configuration_identity": identity, "strategies": []any{strategy}}
	encode := func() string { bytes, _ := json.Marshal(report); return string(bytes) }
	parsed, err := decodeRiskDiagnostics(encode(), 20261007, identity)
	if err != nil || len(parsed.ConfigurationFingerprint) != 64 || *parsed.Strategies[0].Assets["ETH"].ApprovedWeight != -0.2 {
		t.Fatalf("Valid report lost: %v %+v", err, parsed)
	}
	if _, err = decodeRiskDiagnostics(encode(), 20261008, identity); err == nil {
		t.Fatal("Cross-cycle report accepted")
	}
	if _, err = decodeRiskDiagnostics(encode(), 20261007, "different policy"); err == nil {
		t.Fatal("Wrong configuration accepted")
	}
	delete(strategy, "gross_scale")
	if _, err = decodeRiskDiagnostics(encode(), 20261007, identity); err == nil {
		t.Fatal("Missing gross scale became zero")
	}
	strategy["gross_scale"] = 0.8
	strategy["sizing_available"] = false
	if _, err = decodeRiskDiagnostics(encode(), 20261007, identity); err == nil {
		t.Fatal("Unavailable sizing asserted asset evaluations")
	}
	strategy["assets"] = map[string]any{}
	if _, err = decodeRiskDiagnostics(encode(), 20261007, identity); err != nil {
		t.Fatal("Explicit unavailable sizing rejected", err)
	}
}
