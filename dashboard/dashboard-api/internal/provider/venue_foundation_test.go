package provider

import (
	"context"
	"testing"
)

func TestVenueFoundationRequiresAuditedDryRunBoundary(t *testing.T) {
	p := NewReal(RealConfig{
		ExecutionVenue:         "HYPERLIQUID",
		VenueTargetEnvironment: "TESTNET",
		ExchangeGatewayMode:    "hyperliquid-dry-run",
	})
	got := p.VenueFoundation(context.Background())
	if !got.FoundationReady || got.Status != "READY_FOR_PUBLIC_CONNECTIVITY" {
		t.Fatalf("unexpected foundation: %+v", got)
	}
	if got.PublicConnectivity != "NOT_CHECKED" || got.PrivateAuth != "DISABLED" || got.OrderRouting != "DISABLED" {
		t.Fatalf("unsafe Step 33 boundary: %+v", got)
	}
	if got.SecretsRequired || got.CapitalRequired || !got.ReadOnly {
		t.Fatalf("Step 33 must require neither secrets nor capital and must remain read-only: %+v", got)
	}
}

func TestVenueFoundationFailsClosedOnDifferentMode(t *testing.T) {
	p := NewReal(RealConfig{
		ExecutionVenue:         "HYPERLIQUID",
		VenueTargetEnvironment: "TESTNET",
		ExchangeGatewayMode:    "backend",
	})
	got := p.VenueFoundation(context.Background())
	if got.FoundationReady || got.Status != "BLOCKED" {
		t.Fatalf("foundation should fail closed: %+v", got)
	}
}
