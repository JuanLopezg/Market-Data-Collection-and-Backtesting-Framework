package provider

import (
	"context"
	"strings"
)

// VenueFoundation describes the intentionally non-trading boundary prepared in
// Step 33. It contains configuration identity only: no network call, secret,
// signature, private endpoint or order-routing capability is used to build it.
type VenueFoundation struct {
	Status             string `json:"status"`
	FoundationReady    bool   `json:"foundationReady"`
	Venue              string `json:"venue"`
	TargetEnvironment  string `json:"targetEnvironment"`
	GatewayMode        string `json:"gatewayMode"`
	PublicConnectivity string `json:"publicConnectivity"`
	PrivateAuth        string `json:"privateAuth"`
	OrderRouting       string `json:"orderRouting"`
	SymbolMapping      string `json:"symbolMapping"`
	ExchangeFilters    string `json:"exchangeFilters"`
	SecretsRequired    bool   `json:"secretsRequired"`
	CapitalRequired    bool   `json:"capitalRequired"`
	ReadOnly           bool   `json:"readOnly"`
	SourceContract     string `json:"sourceContract"`
	Note               string `json:"note"`
}

func (p *Real) VenueFoundation(_ context.Context) VenueFoundation {
	venue := strings.ToUpper(strings.TrimSpace(p.cfg.ExecutionVenue))
	target := strings.ToUpper(strings.TrimSpace(p.cfg.VenueTargetEnvironment))
	gatewayMode := strings.ToLower(strings.TrimSpace(p.cfg.ExchangeGatewayMode))

	ready := venue == "HYPERLIQUID" && target == "TESTNET" && gatewayMode == "hyperliquid-dry-run"
	status := "BLOCKED"
	if ready {
		status = "READY_FOR_PUBLIC_CONNECTIVITY"
	}

	return VenueFoundation{
		Status:             status,
		FoundationReady:    ready,
		Venue:              venue,
		TargetEnvironment:  target,
		GatewayMode:        gatewayMode,
		PublicConnectivity: "NOT_CHECKED",
		PrivateAuth:        "DISABLED",
		OrderRouting:       "DISABLED",
		SymbolMapping:      "NOT_CONFIGURED",
		ExchangeFilters:    "NOT_LOADED",
		SecretsRequired:    false,
		CapitalRequired:    false,
		ReadOnly:           true,
		SourceContract:     "live_trading/exchange_gateway: hyperliquid-dry-run boundary",
		Note:               "Step 33 establishes venue identity and a fail-closed testnet boundary only. Step 34 performs public connectivity/metadata checks through a separate fail-closed endpoint. No credentials, signing, private API, order submit/cancel or capital are used here.",
	}
}

func (p *EmbeddedMock) VenueFoundation(_ context.Context) VenueFoundation {
	return VenueFoundation{
		Status:             "MOCK_ONLY",
		FoundationReady:    false,
		Venue:              "MOCK",
		TargetEnvironment:  "MOCK",
		GatewayMode:        "mock",
		PublicConnectivity: "NOT_CHECKED",
		PrivateAuth:        "DISABLED",
		OrderRouting:       "DISABLED",
		SymbolMapping:      "NOT_CONFIGURED",
		ExchangeFilters:    "NOT_LOADED",
		SecretsRequired:    false,
		CapitalRequired:    false,
		ReadOnly:           true,
		SourceContract:     "mock provider",
		Note:               "Mock mode cannot satisfy the Step 33 testnet foundation gate.",
	}
}
