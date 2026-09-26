package provider

import (
	"context"
	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	"sort"
	"strings"
	"time"
)

type VenueSymbolMappingRow struct {
	Internal string `json:"internal"`
	Venue    string `json:"venue"`
	State    string `json:"state"`
	Reason   string `json:"reason,omitempty"`
}

type VenueSymbolMappingStatus struct {
	Status                       string                  `json:"status"`
	Validated                    bool                    `json:"validated"`
	ExecutionCoverageComplete    bool                    `json:"executionCoverageComplete"`
	Venue                        string                  `json:"venue"`
	TargetEnvironment            string                  `json:"targetEnvironment"`
	Policy                       string                  `json:"policy"`
	MappingArtifactSHA256        string                  `json:"mappingArtifactSha256"`
	MappingEntryCount            int                     `json:"mappingEntryCount"`
	UnsupportedCatalogEntryCount int                     `json:"unsupportedCatalogEntryCount"`
	ClassificationEntryCount     int                     `json:"classificationEntryCount"`
	RequiredSymbolCount          int                     `json:"requiredSymbolCount"`
	ClassifiedRequiredCount      int                     `json:"classifiedRequiredCount"`
	MappedRequiredCount          int                     `json:"mappedRequiredCount"`
	UnsupportedRequiredCount     int                     `json:"unsupportedRequiredCount"`
	MissingInternalSymbols       []string                `json:"missingInternalSymbols"`
	MissingVenueSymbols          []string                `json:"missingVenueSymbols"`
	UnsupportedInternalSymbols   []string                `json:"unsupportedInternalSymbols"`
	Rows                         []VenueSymbolMappingRow `json:"rows"`
	MetadataLatencyMs            int64                   `json:"metadataLatencyMs"`
	CheckedAt                    string                  `json:"checkedAt"`
	PrivateAuth                  string                  `json:"privateAuth"`
	OrderRouting                 string                  `json:"orderRouting"`
	ExchangeFilters              string                  `json:"exchangeFilters"`
	ReadOnly                     bool                    `json:"readOnly"`
	Error                        string                  `json:"error,omitempty"`
	Note                         string                  `json:"note"`
}

func buildVenueSymbolMapping(required, venueUniverse []string, manifest hldiag.SymbolMapManifest, hash string) VenueSymbolMappingStatus {
	result := VenueSymbolMappingStatus{
		Status: "BLOCKED", Venue: manifest.Venue, TargetEnvironment: manifest.Environment,
		Policy: manifest.MappingPolicy, MappingArtifactSHA256: hash,
		MappingEntryCount: len(manifest.Mappings), UnsupportedCatalogEntryCount: len(manifest.Unsupported),
		ClassificationEntryCount: len(manifest.Mappings) + len(manifest.Unsupported),
		RequiredSymbolCount:      len(required), PrivateAuth: "DISABLED", OrderRouting: "DISABLED",
		ExchangeFilters: "NOT_APPLIED", ReadOnly: true, CheckedAt: time.Now().UTC().Format(time.RFC3339),
		Note: "Step 35 validates explicit internal-symbol classification for Hyperliquid TESTNET. Every required symbol must be either mapped to a currently available venue coin or explicitly blocked as unsupported. Venue coins absent from current TESTNET metadata are also blocked. No suffix stripping, case conversion or alias guessing is used. UNSUPPORTED/UNMAPPED symbols are never routable.",
	}
	mappedIndex := hldiag.MappingIndex(manifest)
	unsupportedIndex := hldiag.UnsupportedIndex(manifest)
	venueSet := make(map[string]struct{}, len(venueUniverse))
	for _, v := range venueUniverse {
		venueSet[v] = struct{}{}
	}
	seenRequired := map[string]struct{}{}
	for _, raw := range required {
		internal := strings.TrimSpace(raw)
		if internal == "" {
			continue
		}
		if _, exists := seenRequired[internal]; exists {
			continue
		}
		seenRequired[internal] = struct{}{}
		if venue, ok := mappedIndex[internal]; ok {
			if _, exists := venueSet[venue]; !exists {
				result.MissingVenueSymbols = append(result.MissingVenueSymbols, venue)
				result.UnsupportedInternalSymbols = append(result.UnsupportedInternalSymbols, internal)
				result.UnsupportedRequiredCount++
				result.ClassifiedRequiredCount++
				result.Rows = append(result.Rows, VenueSymbolMappingRow{Internal: internal, Venue: venue, State: "UNSUPPORTED_VENUE_ABSENT", Reason: "Explicit mapped venue coin is not present in current Hyperliquid TESTNET metadata; order routing remains blocked."})
				continue
			}
			result.MappedRequiredCount++
			result.ClassifiedRequiredCount++
			result.Rows = append(result.Rows, VenueSymbolMappingRow{Internal: internal, Venue: venue, State: "VALID"})
			continue
		}
		if reason, ok := unsupportedIndex[internal]; ok {
			result.UnsupportedInternalSymbols = append(result.UnsupportedInternalSymbols, internal)
			result.UnsupportedRequiredCount++
			result.ClassifiedRequiredCount++
			result.Rows = append(result.Rows, VenueSymbolMappingRow{Internal: internal, State: "UNSUPPORTED_EXPLICIT", Reason: reason})
			continue
		}
		result.MissingInternalSymbols = append(result.MissingInternalSymbols, internal)
		result.Rows = append(result.Rows, VenueSymbolMappingRow{Internal: internal, State: "MISSING_MAPPING", Reason: "No explicit mapping or unsupported classification exists; fail closed."})
	}
	result.RequiredSymbolCount = len(seenRequired)
	sort.Strings(result.MissingInternalSymbols)
	sort.Strings(result.MissingVenueSymbols)
	sort.Strings(result.UnsupportedInternalSymbols)
	sort.Slice(result.Rows, func(i, j int) bool { return result.Rows[i].Internal < result.Rows[j].Internal })
	if result.RequiredSymbolCount > 0 && len(result.MissingInternalSymbols) == 0 && result.ClassifiedRequiredCount == result.RequiredSymbolCount {
		result.Validated = true
		result.ExecutionCoverageComplete = result.MappedRequiredCount == result.RequiredSymbolCount
		if result.UnsupportedRequiredCount == 0 {
			result.Status = "VALIDATED"
		} else {
			result.Status = "VALIDATED_WITH_UNSUPPORTED"
		}
	}
	return result
}

func (p *Real) VenueSymbolMappingStatus(ctx context.Context) VenueSymbolMappingStatus {
	manifest, hash, err := hldiag.ExplicitSymbolMap()
	if err != nil {
		return VenueSymbolMappingStatus{Status: "BLOCKED", Venue: "HYPERLIQUID", TargetEnvironment: "TESTNET", Policy: "EXPLICIT_ONLY", ReadOnly: true, PrivateAuth: "DISABLED", OrderRouting: "DISABLED", ExchangeFilters: "NOT_APPLIED", CheckedAt: time.Now().UTC().Format(time.RFC3339), Error: err.Error(), Note: "Explicit symbol classification artifact could not be validated."}
	}
	if strings.ToUpper(strings.TrimSpace(p.cfg.ExecutionVenue)) != "HYPERLIQUID" || strings.ToUpper(strings.TrimSpace(p.cfg.VenueTargetEnvironment)) != "TESTNET" || strings.ToLower(strings.TrimSpace(p.cfg.ExchangeGatewayMode)) != "hyperliquid-dry-run" {
		result := buildVenueSymbolMapping(nil, nil, manifest, hash)
		result.Error = "venue foundation no longer matches the audited Hyperliquid TESTNET dry-run boundary"
		return result
	}
	market, err := p.marketDataResource(ctx)
	if err != nil {
		result := buildVenueSymbolMapping(nil, nil, manifest, hash)
		result.Error = "canonical strategy universe unavailable: " + err.Error()
		return result
	}
	required := make([]string, 0, len(market.Universe))
	for _, row := range market.Universe {
		required = append(required, row.Asset)
	}
	venueUniverse, latency, checkedAt, err := p.venuePublic.Universe(ctx)
	if err != nil {
		result := buildVenueSymbolMapping(required, nil, manifest, hash)
		result.MetadataLatencyMs = latency.Milliseconds()
		result.CheckedAt = checkedAt.Format(time.RFC3339)
		result.Validated = false
		result.ExecutionCoverageComplete = false
		result.Status = "BLOCKED"
		result.Error = err.Error()
		return result
	}
	result := buildVenueSymbolMapping(required, venueUniverse, manifest, hash)
	result.MetadataLatencyMs = latency.Milliseconds()
	result.CheckedAt = checkedAt.Format(time.RFC3339)
	return result
}

func (p *EmbeddedMock) VenueSymbolMappingStatus(_ context.Context) VenueSymbolMappingStatus {
	return VenueSymbolMappingStatus{Status: "MOCK_ONLY", Validated: false, Venue: "MOCK", TargetEnvironment: "MOCK", Policy: "EXPLICIT_ONLY", PrivateAuth: "DISABLED", OrderRouting: "DISABLED", ExchangeFilters: "NOT_APPLIED", ReadOnly: true, CheckedAt: "mock", Note: "Mock mode cannot satisfy the Step 35 explicit symbol classification gate."}
}
