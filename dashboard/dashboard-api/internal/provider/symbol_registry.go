package provider

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"time"

	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	registrydiag "control-dashboard-api/internal/integration/symbolregistry"
)

type SymbolRegistryRow struct {
	Internal             string `json:"internal"`
	MarketDataVenue      string `json:"marketDataVenue"`
	MarketDataSymbol     string `json:"marketDataSymbol"`
	ExecutionVenue       string `json:"executionVenue"`
	ExecutionEnvironment string `json:"executionEnvironment"`
	ExecutionSymbol      string `json:"executionSymbol"`
	RegistryState        string `json:"registryState"`
	RuntimeState         string `json:"runtimeState"`
	RoutingPolicy        string `json:"routingPolicy"`
	Reason               string `json:"reason,omitempty"`
	InCurrentRanking     bool   `json:"inCurrentRanking"`
	InCurrentStrategy    bool   `json:"inCurrentStrategy"`
	VenuePresent         bool   `json:"venuePresent"`
}

type SymbolRegistryAlarm struct {
	Severity  string `json:"severity"`
	EventType string `json:"eventType"`
	Asset     string `json:"asset,omitempty"`
	Title     string `json:"title"`
	Detail    string `json:"detail"`
}

type SymbolRegistryStatus struct {
	Status                          string                `json:"status"`
	Validated                       bool                  `json:"validated"`
	RegistryVersion                 string                `json:"registryVersion"`
	RegistryArtifactSHA256          string                `json:"registryArtifactSha256"`
	Policy                          string                `json:"policy"`
	MarketDataSource                string                `json:"marketDataSource"`
	ExecutionVenue                  string                `json:"executionVenue"`
	ExecutionEnvironment            string                `json:"executionEnvironment"`
	RegistryEntryCount              int                   `json:"registryEntryCount"`
	CurrentRankingCount             int                   `json:"currentRankingCount"`
	CurrentRankingRegisteredCount   int                   `json:"currentRankingRegisteredCount"`
	CurrentStrategyCount            int                   `json:"currentStrategyCount"`
	CurrentStrategyRegisteredCount  int                   `json:"currentStrategyRegisteredCount"`
	CurrentStrategyRoutableCount    int                   `json:"currentStrategyRoutableCount"`
	CurrentStrategyNonRoutableCount int                   `json:"currentStrategyNonRoutableCount"`
	ExecutionCoverageComplete       bool                  `json:"executionCoverageComplete"`
	UnregisteredRankingSymbols      []string              `json:"unregisteredRankingSymbols"`
	UnregisteredStrategySymbols     []string              `json:"unregisteredStrategySymbols"`
	VenueAbsentStrategySymbols      []string              `json:"venueAbsentStrategySymbols"`
	ExplicitBlockedStrategySymbols  []string              `json:"explicitBlockedStrategySymbols"`
	MappingDivergences              []string              `json:"mappingDivergences"`
	Alarms                          []SymbolRegistryAlarm `json:"alarms"`
	Rows                            []SymbolRegistryRow   `json:"rows"`
	MetadataLatencyMs               int64                 `json:"metadataLatencyMs"`
	CheckedAt                       string                `json:"checkedAt"`
	PrivateAuth                     string                `json:"privateAuth"`
	OrderRouting                    string                `json:"orderRouting"`
	ReadOnly                        bool                  `json:"readOnly"`
	Error                           string                `json:"error,omitempty"`
	Note                            string                `json:"note"`
}

func compareRegistryWithHyperliquidManifest(reg registrydiag.Registry, manifest hldiag.SymbolMapManifest) []string {
	idx := registrydiag.EntryIndex(reg)
	var divergences []string
	seen := map[string]struct{}{}
	for _, mapping := range manifest.Mappings {
		seen[mapping.Internal] = struct{}{}
		entry, ok := idx[mapping.Internal]
		if !ok {
			divergences = append(divergences, fmt.Sprintf("%s missing from multi-exchange registry", mapping.Internal))
			continue
		}
		leg, ok := registrydiag.FindLeg(entry.Execution, "HYPERLIQUID", "TESTNET")
		if !ok || leg.Status != "MAPPED" || leg.Symbol != mapping.Venue {
			divergences = append(divergences, fmt.Sprintf("%s registry Hyperliquid leg disagrees with Step35 manifest", mapping.Internal))
		}
	}
	for _, blocked := range manifest.Unsupported {
		seen[blocked.Internal] = struct{}{}
		entry, ok := idx[blocked.Internal]
		if !ok {
			divergences = append(divergences, fmt.Sprintf("%s unsupported classification missing from multi-exchange registry", blocked.Internal))
			continue
		}
		leg, ok := registrydiag.FindLeg(entry.Execution, "HYPERLIQUID", "TESTNET")
		if !ok || leg.Status != "BLOCKED_EXPLICIT" || leg.RoutingPolicy != "DENY" {
			divergences = append(divergences, fmt.Sprintf("%s registry blocked classification disagrees with Step35 manifest", blocked.Internal))
		}
	}
	for _, entry := range reg.Entries {
		if _, ok := seen[entry.Internal]; !ok {
			divergences = append(divergences, fmt.Sprintf("%s exists in multi-exchange registry but not Step35 manifest", entry.Internal))
		}
		binance, ok := registrydiag.FindLeg(entry.MarketData, "BINANCE", "SOURCE")
		if !ok || binance.Status != "SUPPORTED" || strings.TrimSpace(binance.Symbol) == "" {
			divergences = append(divergences, fmt.Sprintf("%s has no explicit BINANCE/SOURCE symbol", entry.Internal))
		}
	}
	sort.Strings(divergences)
	return divergences
}

func buildSymbolRegistryStatus(
	reg registrydiag.Registry,
	hash string,
	manifest hldiag.SymbolMapManifest,
	ranking []string,
	strategy []string,
	venueUniverse []string,
	latency time.Duration,
	checkedAt time.Time,
) SymbolRegistryStatus {
	result := SymbolRegistryStatus{
		Status: "BLOCKED", RegistryVersion: reg.RegistryVersion, RegistryArtifactSHA256: hash,
		Policy: reg.Policy, MarketDataSource: reg.MarketDataSource, ExecutionVenue: "HYPERLIQUID", ExecutionEnvironment: "TESTNET",
		RegistryEntryCount: len(reg.Entries), CurrentRankingCount: len(ranking), CurrentStrategyCount: len(strategy),
		MetadataLatencyMs: latency.Milliseconds(), CheckedAt: checkedAt.UTC().Format(time.RFC3339), PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", ReadOnly: true,
		Note: "Step 37A introduces a versioned multi-exchange symbol registry. BINANCE/SOURCE and HYPERLIQUID/TESTNET identities are explicit. Missing, blocked, venue-absent or divergent symbols remain non-routable. Other venues must be added as explicit registry legs; heuristic symbol conversion is forbidden.",
	}
	idx := registrydiag.EntryIndex(reg)
	result.MappingDivergences = compareRegistryWithHyperliquidManifest(reg, manifest)
	venueSet := make(map[string]struct{}, len(venueUniverse))
	for _, symbol := range venueUniverse {
		venueSet[symbol] = struct{}{}
	}
	rankingSet := make(map[string]struct{}, len(ranking))
	strategySet := make(map[string]struct{}, len(strategy))
	for _, symbol := range ranking {
		symbol = strings.TrimSpace(symbol)
		if symbol == "" {
			continue
		}
		rankingSet[symbol] = struct{}{}
		if _, ok := idx[symbol]; ok {
			result.CurrentRankingRegisteredCount++
		} else {
			result.UnregisteredRankingSymbols = append(result.UnregisteredRankingSymbols, symbol)
		}
	}
	for _, symbol := range strategy {
		symbol = strings.TrimSpace(symbol)
		if symbol == "" {
			continue
		}
		strategySet[symbol] = struct{}{}
		entry, ok := idx[symbol]
		if !ok {
			result.UnregisteredStrategySymbols = append(result.UnregisteredStrategySymbols, symbol)
			continue
		}
		result.CurrentStrategyRegisteredCount++
		leg, ok := registrydiag.FindLeg(entry.Execution, "HYPERLIQUID", "TESTNET")
		if !ok {
			result.UnregisteredStrategySymbols = append(result.UnregisteredStrategySymbols, symbol)
			continue
		}
		if leg.Status == "MAPPED" {
			if _, present := venueSet[leg.Symbol]; present {
				result.CurrentStrategyRoutableCount++
			} else {
				result.VenueAbsentStrategySymbols = append(result.VenueAbsentStrategySymbols, symbol)
			}
		} else {
			result.ExplicitBlockedStrategySymbols = append(result.ExplicitBlockedStrategySymbols, symbol)
		}
	}
	result.CurrentStrategyNonRoutableCount = result.CurrentStrategyCount - result.CurrentStrategyRoutableCount
	result.ExecutionCoverageComplete = result.CurrentStrategyCount > 0 && result.CurrentStrategyRoutableCount == result.CurrentStrategyCount

	for _, entry := range registrydiag.SortedEntries(reg) {
		_, inRanking := rankingSet[entry.Internal]
		_, inStrategy := strategySet[entry.Internal]
		if !inRanking && !inStrategy {
			continue
		}
		binance, _ := registrydiag.FindLeg(entry.MarketData, "BINANCE", "SOURCE")
		hl, _ := registrydiag.FindLeg(entry.Execution, "HYPERLIQUID", "TESTNET")
		row := SymbolRegistryRow{Internal: entry.Internal, MarketDataVenue: "BINANCE", MarketDataSymbol: binance.Symbol, ExecutionVenue: "HYPERLIQUID", ExecutionEnvironment: "TESTNET", ExecutionSymbol: hl.Symbol, RegistryState: hl.Status, RoutingPolicy: hl.RoutingPolicy, Reason: hl.Reason, InCurrentRanking: inRanking, InCurrentStrategy: inStrategy}
		if hl.Status == "MAPPED" {
			_, row.VenuePresent = venueSet[hl.Symbol]
			if row.VenuePresent {
				row.RuntimeState = "ROUTABLE_PUBLICLY_VALIDATED"
			} else {
				row.RuntimeState = "VENUE_ABSENT"
			}
		} else {
			row.RuntimeState = "BLOCKED_EXPLICIT"
		}
		result.Rows = append(result.Rows, row)
	}

	sort.Strings(result.UnregisteredRankingSymbols)
	sort.Strings(result.UnregisteredStrategySymbols)
	sort.Strings(result.VenueAbsentStrategySymbols)
	sort.Strings(result.ExplicitBlockedStrategySymbols)

	for _, detail := range result.MappingDivergences {
		result.Alarms = append(result.Alarms, SymbolRegistryAlarm{Severity: "CRITICAL", EventType: "SYMBOL_REGISTRY_DIVERGENCE", Title: "Symbol registry disagrees with the accepted Step 35 manifest", Detail: detail})
	}
	for _, asset := range result.UnregisteredRankingSymbols {
		result.Alarms = append(result.Alarms, SymbolRegistryAlarm{Severity: "CRITICAL", EventType: "SYMBOL_UNREGISTERED_MARKET_DATA", Asset: asset, Title: "Market-data symbol is not registered", Detail: "Current canonical Binance/source ranking contains an asset with no explicit multi-exchange registry entry. It must remain non-routable until classified."})
	}
	for _, asset := range result.UnregisteredStrategySymbols {
		result.Alarms = append(result.Alarms, SymbolRegistryAlarm{Severity: "CRITICAL", EventType: "SYMBOL_UNREGISTERED_STRATEGY", Asset: asset, Title: "Strategy selected an unregistered symbol", Detail: "The current strategy universe contains an asset with no complete registry classification. Order routing must remain blocked."})
	}
	for _, asset := range result.VenueAbsentStrategySymbols {
		result.Alarms = append(result.Alarms, SymbolRegistryAlarm{Severity: "WARN", EventType: "SYMBOL_VENUE_ABSENT", Asset: asset, Title: "Mapped execution symbol is absent from current TESTNET metadata", Detail: "The explicit mapping exists but the venue no longer advertises the mapped coin. The asset remains non-routable until metadata/mapping is reviewed."})
	}
	if len(result.ExplicitBlockedStrategySymbols) > 0 {
		result.Alarms = append(result.Alarms, SymbolRegistryAlarm{Severity: "WARN", EventType: "SYMBOL_COVERAGE_PARTIAL", Title: "Current strategy universe contains explicitly unsupported execution assets", Detail: fmt.Sprintf("%d strategy symbol(s) are explicitly blocked by the registry: %s", len(result.ExplicitBlockedStrategySymbols), strings.Join(result.ExplicitBlockedStrategySymbols, ", "))})
	}

	if len(result.MappingDivergences) == 0 && len(result.UnregisteredRankingSymbols) == 0 && len(result.UnregisteredStrategySymbols) == 0 && result.CurrentStrategyRegisteredCount == result.CurrentStrategyCount && result.CurrentRankingRegisteredCount == result.CurrentRankingCount {
		result.Validated = true
		if len(result.Alarms) == 0 {
			result.Status = "VALIDATED"
		} else {
			result.Status = "VALIDATED_WITH_WARNINGS"
		}
	}
	return result
}

func (p *Real) SymbolRegistryStatus(ctx context.Context) SymbolRegistryStatus {
	reg, hash, err := registrydiag.Load()
	if err != nil {
		return SymbolRegistryStatus{Status: "BLOCKED", Policy: "EXPLICIT_ONLY", MarketDataSource: "BINANCE", ExecutionVenue: "HYPERLIQUID", ExecutionEnvironment: "TESTNET", CheckedAt: time.Now().UTC().Format(time.RFC3339), PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", ReadOnly: true, Error: err.Error(), Note: "Multi-exchange symbol registry could not be loaded; all execution routing must remain disabled."}
	}
	manifest, _, err := hldiag.ExplicitSymbolMap()
	if err != nil {
		return SymbolRegistryStatus{Status: "BLOCKED", RegistryVersion: reg.RegistryVersion, RegistryArtifactSHA256: hash, Policy: reg.Policy, MarketDataSource: reg.MarketDataSource, ExecutionVenue: "HYPERLIQUID", ExecutionEnvironment: "TESTNET", RegistryEntryCount: len(reg.Entries), CheckedAt: time.Now().UTC().Format(time.RFC3339), PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", ReadOnly: true, Error: err.Error(), Note: "Step35 symbol manifest is unavailable; registry consistency cannot be established."}
	}
	window, err := p.marketData.LoadStrategyWindow(ctx, maxInt(p.cfg.MarketTopN, 50), maxInt(p.cfg.MarketHistoryDays, 100))
	if err != nil {
		result := buildSymbolRegistryStatus(reg, hash, manifest, nil, nil, nil, 0, time.Now().UTC())
		result.Error = "canonical market-data ranking unavailable: " + err.Error()
		result.Status = "BLOCKED"
		result.Validated = false
		return result
	}
	ranking := make([]string, 0, len(window.Ranking))
	for _, row := range window.Ranking {
		ranking = append(ranking, row.Pair)
	}
	market, err := p.marketDataResource(ctx)
	if err != nil {
		result := buildSymbolRegistryStatus(reg, hash, manifest, ranking, nil, nil, 0, time.Now().UTC())
		result.Error = "current strategy universe unavailable: " + err.Error()
		result.Status = "BLOCKED"
		result.Validated = false
		return result
	}
	strategy := make([]string, 0, len(market.Universe))
	for _, row := range market.Universe {
		strategy = append(strategy, row.Asset)
	}
	venueUniverse, latency, checkedAt, err := p.venuePublic.Universe(ctx)
	if err != nil {
		result := buildSymbolRegistryStatus(reg, hash, manifest, ranking, strategy, nil, latency, checkedAt)
		result.Error = "current Hyperliquid TESTNET metadata unavailable: " + err.Error()
		result.Status = "BLOCKED"
		result.Validated = false
		return result
	}
	return buildSymbolRegistryStatus(reg, hash, manifest, ranking, strategy, venueUniverse, latency, checkedAt)
}

func (p *EmbeddedMock) SymbolRegistryStatus(_ context.Context) SymbolRegistryStatus {
	return SymbolRegistryStatus{Status: "MOCK_ONLY", Validated: false, Policy: "EXPLICIT_ONLY", MarketDataSource: "BINANCE", ExecutionVenue: "HYPERLIQUID", ExecutionEnvironment: "TESTNET", CheckedAt: "mock", PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", ReadOnly: true, Note: "Mock mode cannot validate the real multi-exchange symbol registry."}
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
