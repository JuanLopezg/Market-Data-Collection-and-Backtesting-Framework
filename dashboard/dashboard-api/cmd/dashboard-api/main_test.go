package main

import (
	"context"
	"testing"

	"control-dashboard-api/internal/provider"
)

func TestConfiguredProviderStep34DefaultsToAuditedVenueBoundary(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "")
	t.Setenv("DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL", "")
	t.Setenv("DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT", "")
	p, err := configuredProvider()
	if err != nil {
		t.Fatalf("configuredProvider() error = %v", err)
	}
	reader, ok := p.(interface {
		VenueFoundation(context.Context) provider.VenueFoundation
	})
	if !ok {
		t.Fatal("real provider does not expose venue foundation")
	}
	got := reader.VenueFoundation(context.Background())
	if !got.FoundationReady || got.Venue != "HYPERLIQUID" || got.TargetEnvironment != "TESTNET" || got.GatewayMode != "hyperliquid-dry-run" {
		t.Fatalf("unexpected Step 34 defaults: %+v", got)
	}
}

func TestConfiguredProviderRejectsUnauditedVenueInStep34(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "BINANCE")
	if _, err := configuredProvider(); err == nil {
		t.Fatal("configuredProvider() accepted an unaudited Step 34 venue")
	}
}

func TestConfiguredProviderRejectsTradingGatewayModeInStep34(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "backend")
	if _, err := configuredProvider(); err == nil {
		t.Fatal("configuredProvider() accepted backend mode during Step 34")
	}
}

func TestValidateHyperliquidTestnetInfoURL(t *testing.T) {
	if err := validateHyperliquidTestnetInfoURL("https://api.hyperliquid-testnet.xyz/info"); err != nil {
		t.Fatalf("expected official testnet info URL to pass: %v", err)
	}
	bad := []string{
		"http://api.hyperliquid-testnet.xyz/info",
		"https://api.hyperliquid.xyz/info",
		"https://api.hyperliquid-testnet.xyz/exchange",
		"https://example.com/info",
		"https://api.hyperliquid-testnet.xyz/info?x=1",
	}
	for _, value := range bad {
		if err := validateHyperliquidTestnetInfoURL(value); err == nil {
			t.Fatalf("accepted unsafe public endpoint %q", value)
		}
	}
}
