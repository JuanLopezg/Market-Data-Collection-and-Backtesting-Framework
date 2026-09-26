package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func manualCheckpoint() pgstore.PipelineCheckpoint {
	return pgstore.PipelineCheckpoint{
		Timestamp: 20260919,
		Account: tradingwire.AccountSnapshot{
			Metadata:  tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "account"},
			Timestamp: 20260919,
			Cash:      20000,
			Positions: map[string]float64{},
		},
		Decision: tradingwire.DecisionBatch{
			Metadata:          tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "decision"},
			DecisionTimestamp: 20260919,
			Strategies: []tradingwire.StrategyDecisionIntent{{
				StrategyID:        1,
				DecisionTimestamp: 20260919,
				ReferenceCapital:  100000,
				TargetNotionalUSD: map[string]float64{"BTCUSDT": 50000, "ETHUSDT": 30000},
				Decisions: []tradingwire.RebalanceDecision{
					{Coin: "BTCUSDT", TargetWeight: 0.5},
					{Coin: "ETHUSDT", TargetWeight: 0.3},
				},
			}},
		},
	}
}

func TestManualPreviewAcceptsCanonicalPortfolioAndStaysNonExecuting(t *testing.T) {
	result, requested := buildManualPreviewWithPairs(
		manualCheckpoint(),
		[]string{"BTCUSDT", "ETHUSDT", "SOLUSDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\nBTCUSDT,40\nETHUSDT,30\nSOLUSDT,10\nCASH,20\n"},
	)
	if !result.ValidationPassed {
		t.Fatalf("validation failed: %+v", result.ValidationIssues)
	}
	if len(requested) != 4 || requested["CASH"] != 20 {
		t.Fatalf("unexpected requested portfolio: %+v", requested)
	}
	if result.RouteEnabled || result.RiskCheckAvailable {
		t.Fatal("manual preview must not imply risk approval or routing")
	}
	if got := result.EstimatedTurnoverLabel; got != "10.00% target delta" {
		t.Fatalf("turnover = %q", got)
	}
}

func TestManualPreviewRejectsUnknownAssetAndBadWeightSum(t *testing.T) {
	result, _ := buildManualPreviewWithPairs(
		manualCheckpoint(),
		[]string{"BTCUSDT", "ETHUSDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\nBTCUSDT,50\nUNKNOWNUSDT,20\nCASH,20\n"},
	)
	if result.ValidationPassed {
		t.Fatal("invalid portfolio unexpectedly passed")
	}
	errors := 0
	for _, issue := range result.ValidationIssues {
		if issue.Severity == "ERROR" {
			errors++
		}
	}
	if errors < 2 {
		t.Fatalf("expected multiple validation errors, got %+v", result.ValidationIssues)
	}
}

func TestCanonicalManualPortfolioHashInputIsOrderIndependent(t *testing.T) {
	left := canonicalManualPortfolio(map[string]float64{"BTCUSDT": 50, "CASH": 50})
	right := canonicalManualPortfolio(map[string]float64{"CASH": 50, "BTCUSDT": 50})
	if left != right {
		t.Fatalf("canonical portfolio differs: %q vs %q", left, right)
	}
}

func TestManualPreviewTreatsOmittedCurrentAssetAsTargetZero(t *testing.T) {
	result, _ := buildManualPreviewWithPairs(
		manualCheckpoint(),
		[]string{"BTCUSDT", "ETHUSDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\nBTCUSDT,60\nCASH,40\n"},
	)
	if !result.ValidationPassed {
		t.Fatalf("validation failed: %+v", result.ValidationIssues)
	}
	found := false
	for _, row := range result.OrderPreview {
		if row.Asset != "ETHUSDT" {
			continue
		}
		found = true
		if row.Action != "SELL" || row.DeltaWeightPct != -30 || row.EstimatedNotionalUSD != 30000 {
			t.Fatalf("omitted ETH target must be an explicit sell-to-zero delta: %+v", row)
		}
	}
	if !found {
		t.Fatalf("omitted current asset ETHUSDT missing from order preview: %+v", result.OrderPreview)
	}
}

func TestManualExecutionConstraintsRequireExplicitRoutableRegistryAndRules(t *testing.T) {
	result, _ := buildManualPreviewWithPairs(
		manualCheckpoint(),
		[]string{"BTCUSDT", "ETHUSDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\nBTCUSDT,50\nETHUSDT,30\nCASH,20\n"},
	)
	if !result.ValidationPassed {
		t.Fatalf("base validation failed: %+v", result.ValidationIssues)
	}
	registry := SymbolRegistryStatus{
		Validated:              true,
		RegistryVersion:        "test-registry",
		RegistryArtifactSHA256: "abc",
		Rows: []SymbolRegistryRow{
			{Internal: "BTCUSDT", RuntimeState: "ROUTABLE_PUBLICLY_VALIDATED", RoutingPolicy: "ALLOW_IF_VENUE_PRESENT_AND_RULES_VALID"},
			{Internal: "ETHUSDT", RuntimeState: "ROUTABLE_PUBLICLY_VALIDATED", RoutingPolicy: "ALLOW_IF_VENUE_PRESENT_AND_RULES_VALID"},
		},
	}
	rules := VenueTradingRulesStatus{
		Validated: true,
		CheckedAt: "2026-09-26T00:00:00Z",
		Rows: []VenueTradingRuleRow{
			{Internal: "BTCUSDT", State: "VALID", MinOrderNotionalUSD: 10},
			{Internal: "ETHUSDT", State: "VALID", MinOrderNotionalUSD: 10},
		},
	}
	applyManualExecutionConstraints(&result, registry, rules)
	if !result.ValidationPassed || !result.ExchangeConstraintsValidated {
		t.Fatalf("explicit routable registry/rules should validate execution constraints: %+v", result)
	}

	blocked, _ := buildManualPreviewWithPairs(
		manualCheckpoint(),
		[]string{"BTCUSDT", "ETHUSDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\nBTCUSDT,60\nCASH,40\n"},
	)
	blockedRegistry := registry
	blockedRegistry.Rows = []SymbolRegistryRow{
		{Internal: "BTCUSDT", RuntimeState: "ROUTABLE_PUBLICLY_VALIDATED", RoutingPolicy: "ALLOW_IF_VENUE_PRESENT_AND_RULES_VALID"},
		{Internal: "ETHUSDT", RuntimeState: "BLOCKED_EXPLICIT", RoutingPolicy: "DENY"},
	}
	applyManualExecutionConstraints(&blocked, blockedRegistry, rules)
	if blocked.ValidationPassed || blocked.ExchangeConstraintsValidated {
		t.Fatalf("non-routable omitted-current sell must fail closed: %+v", blocked)
	}
	foundBlocker := false
	for _, blocker := range blocked.RouteBlockers {
		if blocker == "ASSET_NOT_ROUTABLE:ETHUSDT" {
			foundBlocker = true
		}
	}
	if !foundBlocker {
		t.Fatalf("missing explicit non-routable blocker: %+v", blocked.RouteBlockers)
	}
}

func TestManualPreviewAcceptsExactKnownUnicodeCanonicalAsset(t *testing.T) {
	checkpoint := manualCheckpoint()
	checkpoint.Decision.Strategies[0].Decisions = []tradingwire.RebalanceDecision{{Coin: "龙虾USDT", TargetWeight: 0.1}}
	checkpoint.Decision.Strategies[0].TargetNotionalUSD = map[string]float64{"龙虾USDT": 10000}
	result, requested := buildManualPreviewWithPairs(
		checkpoint,
		[]string{"龙虾USDT"},
		ManualControlPreviewRequest{CSV: "asset,weight_pct\n龙虾USDT,10\nCASH,90\n"},
	)
	if !result.ValidationPassed || requested["龙虾USDT"] != 10 {
		t.Fatalf("exact known Unicode canonical asset should pass structural validation: result=%+v requested=%+v", result, requested)
	}
}
