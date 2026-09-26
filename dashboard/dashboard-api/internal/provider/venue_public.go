package provider

import (
	"context"
	"strings"
	"time"
)

// VenuePublicStatus is a bounded read-only view of public exchange reachability
// and metadata. It deliberately excludes account state, credentials, signing,
// order submission/cancel and any capital-moving capability.
type VenuePublicStatus struct {
	Status             string   `json:"status"`
	Connected          bool     `json:"connected"`
	Venue              string   `json:"venue"`
	TargetEnvironment  string   `json:"targetEnvironment"`
	Endpoint           string   `json:"endpoint"`
	MetadataStatus     string   `json:"metadataStatus"`
	MidsStatus         string   `json:"midsStatus"`
	UniverseCount      int      `json:"universeCount"`
	MidCount           int      `json:"midCount"`
	MatchedMidCount    int      `json:"matchedMidCount"`
	SampleSymbols      []string `json:"sampleSymbols"`
	LatencyMs          int64    `json:"latencyMs"`
	CheckedAt          string   `json:"checkedAt"`
	PrivateAuth        string   `json:"privateAuth"`
	OrderRouting       string   `json:"orderRouting"`
	SymbolMapping      string   `json:"symbolMapping"`
	ExchangeFilters    string   `json:"exchangeFilters"`
	SecretsUsed        bool     `json:"secretsUsed"`
	CapitalUsed        bool     `json:"capitalUsed"`
	ReadOnly           bool     `json:"readOnly"`
	Error              string   `json:"error,omitempty"`
	Note               string   `json:"note"`
}

func (p *Real) VenuePublicStatus(ctx context.Context) VenuePublicStatus {
	result := VenuePublicStatus{
		Status:            "BLOCKED",
		Venue:             strings.ToUpper(strings.TrimSpace(p.cfg.ExecutionVenue)),
		TargetEnvironment: strings.ToUpper(strings.TrimSpace(p.cfg.VenueTargetEnvironment)),
		Endpoint:          strings.TrimSpace(p.cfg.VenuePublicInfoURL),
		MetadataStatus:    "UNAVAILABLE",
		MidsStatus:        "UNAVAILABLE",
		PrivateAuth:       "DISABLED",
		OrderRouting:      "DISABLED",
		SymbolMapping:     "NOT_CONFIGURED",
		ExchangeFilters:   "NOT_APPLIED",
		SecretsUsed:       false,
		CapitalUsed:       false,
		ReadOnly:          true,
		CheckedAt:         time.Now().UTC().Format(time.RFC3339),
		Note:              "Step 34 performs two fixed public Hyperliquid TESTNET info queries (meta and allMids). No user/account endpoint, secret, signature, order submit/cancel or capital is used. Symbol mapping and venue trading rules remain future gates.",
	}

	if result.Venue != "HYPERLIQUID" || result.TargetEnvironment != "TESTNET" || strings.ToLower(strings.TrimSpace(p.cfg.ExchangeGatewayMode)) != "hyperliquid-dry-run" {
		result.Error = "venue foundation no longer matches the audited Hyperliquid TESTNET dry-run boundary"
		return result
	}
	if p.venuePublic == nil {
		result.Error = "public venue client is not configured"
		return result
	}

	checked, err := p.venuePublic.Check(ctx)
	result.CheckedAt = checked.CheckedAt.Format(time.RFC3339)
	result.LatencyMs = checked.Latency.Milliseconds()
	result.UniverseCount = checked.UniverseCount
	result.MidCount = checked.MidCount
	result.MatchedMidCount = checked.MatchedMidCount
	result.SampleSymbols = checked.SampleSymbols
	if checked.MetadataReady {
		result.MetadataStatus = "AVAILABLE"
	}
	if checked.MidsReady {
		result.MidsStatus = "AVAILABLE"
	}
	if err != nil {
		result.Error = err.Error()
		return result
	}

	result.Connected = checked.Connected
	if checked.Connected {
		result.Status = "CONNECTED_PUBLIC_ONLY"
	}
	return result
}

func (p *EmbeddedMock) VenuePublicStatus(_ context.Context) VenuePublicStatus {
	return VenuePublicStatus{
		Status:            "MOCK_ONLY",
		Connected:         false,
		Venue:             "MOCK",
		TargetEnvironment: "MOCK",
		Endpoint:          "",
		MetadataStatus:    "NOT_CHECKED",
		MidsStatus:        "NOT_CHECKED",
		PrivateAuth:       "DISABLED",
		OrderRouting:      "DISABLED",
		SymbolMapping:     "NOT_CONFIGURED",
		ExchangeFilters:   "NOT_APPLIED",
		SecretsUsed:       false,
		CapitalUsed:       false,
		ReadOnly:          true,
		CheckedAt:         "mock",
		Note:              "Mock mode does not contact Hyperliquid and cannot satisfy the Step 34 public venue connectivity gate.",
	}
}
