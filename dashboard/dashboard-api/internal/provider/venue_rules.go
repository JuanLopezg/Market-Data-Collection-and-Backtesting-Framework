package provider

import (
	"context"
	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	hyperliquidPerpPriceMaxSignificantFigures = 5
	hyperliquidPerpMaxDecimals                = 6
	hyperliquidMinOrderNotionalUSD            = 10.0
)

type VenueTradingRuleRow struct {
	Internal                     string  `json:"internal"`
	Venue                        string  `json:"venue"`
	State                        string  `json:"state"`
	SizeDecimals                 int     `json:"sizeDecimals"`
	SizeStepLabel                string  `json:"sizeStepLabel"`
	PriceMaxSignificantFigures   int     `json:"priceMaxSignificantFigures"`
	PriceMaxDecimals             int     `json:"priceMaxDecimals"`
	MaxLeverage                  int     `json:"maxLeverage"`
	OnlyIsolated                 bool    `json:"onlyIsolated"`
	MarginMode                   string  `json:"marginMode,omitempty"`
	IsDelisted                   bool    `json:"isDelisted"`
	MidPrice                     string  `json:"midPrice"`
	MinOrderNotionalUSD          float64 `json:"minOrderNotionalUsd"`
	EstimatedMinOrderSizeLabel   string  `json:"estimatedMinOrderSizeLabel"`
	EstimatedMinOrderNotionalUSD float64 `json:"estimatedMinOrderNotionalUsd"`
	Reason                       string  `json:"reason,omitempty"`
}

type VenueTradingRulesStatus struct {
	Status                     string                `json:"status"`
	Validated                  bool                  `json:"validated"`
	Venue                      string                `json:"venue"`
	TargetEnvironment          string                `json:"targetEnvironment"`
	SupportedMappingCount      int                   `json:"supportedMappingCount"`
	NonRoutableMappingCount    int                   `json:"nonRoutableMappingCount"`
	ValidatedRuleCount         int                   `json:"validatedRuleCount"`
	BlockedRuleCount           int                   `json:"blockedRuleCount"`
	PriceMaxSignificantFigures int                   `json:"priceMaxSignificantFigures"`
	PerpMaxDecimals            int                   `json:"perpMaxDecimals"`
	IntegerPricesAlwaysAllowed bool                  `json:"integerPricesAlwaysAllowed"`
	MinOrderNotionalUSD        float64               `json:"minOrderNotionalUsd"`
	Rows                       []VenueTradingRuleRow `json:"rows"`
	MetadataLatencyMs          int64                 `json:"metadataLatencyMs"`
	CheckedAt                  string                `json:"checkedAt"`
	PrivateAuth                string                `json:"privateAuth"`
	OrderRouting               string                `json:"orderRouting"`
	ReadOnly                   bool                  `json:"readOnly"`
	RuleSource                 string                `json:"ruleSource"`
	Error                      string                `json:"error,omitempty"`
	Note                       string                `json:"note"`
}

func buildVenueTradingRules(mapping VenueSymbolMappingStatus, publicRules []hldiag.PublicPerpRule) VenueTradingRulesStatus {
	result := VenueTradingRulesStatus{
		Status:                     "BLOCKED",
		Venue:                      mapping.Venue,
		TargetEnvironment:          mapping.TargetEnvironment,
		SupportedMappingCount:      mapping.MappedRequiredCount,
		NonRoutableMappingCount:    mapping.UnsupportedRequiredCount,
		PriceMaxSignificantFigures: hyperliquidPerpPriceMaxSignificantFigures,
		PerpMaxDecimals:            hyperliquidPerpMaxDecimals,
		IntegerPricesAlwaysAllowed: true,
		MinOrderNotionalUSD:        hyperliquidMinOrderNotionalUSD,
		PrivateAuth:                "DISABLED",
		OrderRouting:               "DISABLED",
		ReadOnly:                   true,
		CheckedAt:                  time.Now().UTC().Format(time.RFC3339),
		RuleSource:                 "Hyperliquid public TESTNET meta + allMids; documented perp precision/min-notional protocol rules",
		Note:                       "Step 36 validates public trading constraints only for Step 35-supported mappings. Unsupported/unmapped assets remain non-routable. These diagnostics do not round or submit any order and must be revalidated against the final production venue before LIVE capital.",
	}
	if !mapping.Validated {
		result.Error = "Step 35 explicit symbol classification is not validated"
		return result
	}
	if mapping.MappedRequiredCount <= 0 {
		result.Error = "Step 35 produced no supported mappings to validate"
		return result
	}

	byVenue := make(map[string]hldiag.PublicPerpRule, len(publicRules))
	for _, rule := range publicRules {
		if rule.Name == "" {
			continue
		}
		byVenue[rule.Name] = rule
	}

	for _, mapped := range mapping.Rows {
		if mapped.State != "VALID" {
			continue
		}
		row := VenueTradingRuleRow{
			Internal:                   mapped.Internal,
			Venue:                      mapped.Venue,
			State:                      "BLOCKED",
			PriceMaxSignificantFigures: hyperliquidPerpPriceMaxSignificantFigures,
			MinOrderNotionalUSD:        hyperliquidMinOrderNotionalUSD,
		}
		rule, ok := byVenue[mapped.Venue]
		if !ok {
			row.Reason = "Mapped venue coin is absent from the current public rules snapshot."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		row.SizeDecimals = rule.SzDecimals
		row.MaxLeverage = rule.MaxLeverage
		row.OnlyIsolated = rule.OnlyIsolated
		row.MarginMode = rule.MarginMode
		row.IsDelisted = rule.IsDelisted
		row.MidPrice = rule.Mid

		if rule.SzDecimals < 0 || rule.SzDecimals > hyperliquidPerpMaxDecimals {
			row.Reason = fmt.Sprintf("Invalid szDecimals=%d for a perp; cannot derive a safe public precision rule.", rule.SzDecimals)
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		row.PriceMaxDecimals = hyperliquidPerpMaxDecimals - rule.SzDecimals
		row.SizeStepLabel = decimalStepLabel(rule.SzDecimals)
		if rule.MaxLeverage <= 0 {
			row.Reason = "Public metadata has no positive maxLeverage."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		if rule.IsDelisted {
			row.Reason = "Venue marks this perpetual as delisted; routing must remain blocked."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		mid, err := strconv.ParseFloat(strings.TrimSpace(rule.Mid), 64)
		if err != nil || !isFinitePositive(mid) {
			row.Reason = "Current public mid is unavailable or invalid; minimum executable size cannot be estimated safely."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		minSize := roundSizeUp(hyperliquidMinOrderNotionalUSD/mid, rule.SzDecimals)
		if !isFinitePositive(minSize) {
			row.Reason = "Could not derive a finite minimum order size estimate."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		row.EstimatedMinOrderSizeLabel = formatRuleQuantity(minSize, rule.SzDecimals)
		row.EstimatedMinOrderNotionalUSD = minSize * mid
		if row.EstimatedMinOrderNotionalUSD+1e-9 < hyperliquidMinOrderNotionalUSD {
			row.Reason = "Rounded minimum-size estimate falls below the documented minimum order notional."
			result.BlockedRuleCount++
			result.Rows = append(result.Rows, row)
			continue
		}
		row.State = "VALID"
		row.Reason = "Public precision/leverage metadata is usable for future testnet order validation; no routing is enabled."
		result.ValidatedRuleCount++
		result.Rows = append(result.Rows, row)
	}

	sort.Slice(result.Rows, func(i, j int) bool { return result.Rows[i].Internal < result.Rows[j].Internal })
	if len(result.Rows) != mapping.MappedRequiredCount {
		result.Error = fmt.Sprintf("supported mapping/rule row mismatch: rows=%d mappings=%d", len(result.Rows), mapping.MappedRequiredCount)
		return result
	}
	if result.BlockedRuleCount > 0 || result.ValidatedRuleCount != mapping.MappedRequiredCount {
		result.Error = fmt.Sprintf("public trading-rule validation incomplete: validated=%d/%d blocked=%d", result.ValidatedRuleCount, mapping.MappedRequiredCount, result.BlockedRuleCount)
		return result
	}
	result.Validated = true
	result.Status = "VALIDATED"
	return result
}

func decimalStepLabel(decimals int) string {
	if decimals <= 0 {
		return "1"
	}
	return "0." + strings.Repeat("0", decimals-1) + "1"
}

func roundSizeUp(value float64, decimals int) float64 {
	scale := math.Pow10(decimals)
	return math.Ceil(value*scale-1e-12) / scale
}

func formatRuleQuantity(value float64, decimals int) string {
	return strconv.FormatFloat(value, 'f', decimals, 64)
}

func isFinitePositive(value float64) bool {
	return value > 0 && !math.IsNaN(value) && !math.IsInf(value, 0)
}

func (p *Real) VenueTradingRulesStatus(ctx context.Context) VenueTradingRulesStatus {
	manifest, hash, err := hldiag.ExplicitSymbolMap()
	if err != nil {
		mapping := VenueSymbolMappingStatus{Status: "BLOCKED", Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET"}
		result := buildVenueTradingRules(mapping, nil)
		result.Error = "explicit symbol classification artifact could not be validated: " + err.Error()
		return result
	}
	if strings.ToUpper(strings.TrimSpace(p.cfg.ExecutionVenue)) != "HYPERLIQUID" || strings.ToUpper(strings.TrimSpace(p.cfg.VenueTargetEnvironment)) != "TESTNET" || strings.ToLower(strings.TrimSpace(p.cfg.ExchangeGatewayMode)) != "hyperliquid-dry-run" {
		mapping := buildVenueSymbolMapping(nil, nil, manifest, hash)
		result := buildVenueTradingRules(mapping, nil)
		result.Error = "venue foundation no longer matches the audited Hyperliquid TESTNET dry-run boundary"
		return result
	}
	market, err := p.marketDataResource(ctx)
	if err != nil {
		mapping := buildVenueSymbolMapping(nil, nil, manifest, hash)
		result := buildVenueTradingRules(mapping, nil)
		result.Error = "canonical strategy universe unavailable: " + err.Error()
		return result
	}
	required := make([]string, 0, len(market.Universe))
	for _, row := range market.Universe {
		required = append(required, row.Asset)
	}

	rules, latency, checkedAt, err := p.venuePublic.PerpRules(ctx)
	if err != nil {
		mapping := buildVenueSymbolMapping(required, nil, manifest, hash)
		result := buildVenueTradingRules(mapping, nil)
		result.MetadataLatencyMs = latency.Milliseconds()
		result.CheckedAt = checkedAt.Format(time.RFC3339)
		result.Error = err.Error()
		return result
	}
	venueUniverse := make([]string, 0, len(rules))
	for _, rule := range rules {
		venueUniverse = append(venueUniverse, rule.Name)
	}
	mapping := buildVenueSymbolMapping(required, venueUniverse, manifest, hash)
	if !mapping.Validated {
		result := buildVenueTradingRules(mapping, nil)
		result.MetadataLatencyMs = latency.Milliseconds()
		result.CheckedAt = checkedAt.Format(time.RFC3339)
		if len(mapping.MissingInternalSymbols) > 0 {
			result.Error = "Step 35 classification is incomplete for the current canonical strategy universe"
		} else {
			result.Error = "Step 35 classification could not be validated against the current public rule snapshot"
		}
		return result
	}
	result := buildVenueTradingRules(mapping, rules)
	result.MetadataLatencyMs = latency.Milliseconds()
	result.CheckedAt = checkedAt.Format(time.RFC3339)
	return result
}

func (p *EmbeddedMock) VenueTradingRulesStatus(_ context.Context) VenueTradingRulesStatus {
	return VenueTradingRulesStatus{
		Status: "MOCK_ONLY", Venue: "MOCK", TargetEnvironment: "MOCK",
		PriceMaxSignificantFigures: hyperliquidPerpPriceMaxSignificantFigures,
		PerpMaxDecimals:            hyperliquidPerpMaxDecimals, IntegerPricesAlwaysAllowed: true, MinOrderNotionalUSD: hyperliquidMinOrderNotionalUSD,
		PrivateAuth: "DISABLED", OrderRouting: "DISABLED", ReadOnly: true, CheckedAt: "mock",
		RuleSource: "mock provider", Note: "Mock mode cannot satisfy the Step 36 public venue trading-rules gate.",
	}
}
