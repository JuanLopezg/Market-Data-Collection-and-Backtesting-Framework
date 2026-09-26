package provider

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestVenuePublicStatusUsesPublicOnlyQueries(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var input map[string]any
		if err := json.NewDecoder(r.Body).Decode(&input); err != nil {
			t.Fatal(err)
		}
		switch input["type"] {
		case "meta":
			_, _ = w.Write([]byte(`{"universe":[{"name":"BTC"},{"name":"ETH"}]}`))
		case "allMids":
			_, _ = w.Write([]byte(`{"BTC":"100000","ETH":"4000"}`))
		default:
			http.Error(w, "unexpected", http.StatusBadRequest)
		}
	}))
	defer server.Close()

	p := NewReal(RealConfig{
		ExecutionVenue:         "HYPERLIQUID",
		VenueTargetEnvironment: "TESTNET",
		ExchangeGatewayMode:    "hyperliquid-dry-run",
		VenuePublicInfoURL:     server.URL,
		VenuePublicTimeout:     time.Second,
	})
	got := p.VenuePublicStatus(context.Background())
	if got.Status != "CONNECTED_PUBLIC_ONLY" || !got.Connected {
		t.Fatalf("unexpected status: %+v", got)
	}
	if got.PrivateAuth != "DISABLED" || got.OrderRouting != "DISABLED" || got.SecretsUsed || got.CapitalUsed || !got.ReadOnly {
		t.Fatalf("unsafe public venue status: %+v", got)
	}
	if got.SymbolMapping != "NOT_CONFIGURED" || got.ExchangeFilters != "NOT_APPLIED" {
		t.Fatalf("Step 34 must not claim future gates: %+v", got)
	}
}

func TestVenuePublicStatusFailsClosedWhenProbeFails(t *testing.T) {
	p := NewReal(RealConfig{
		ExecutionVenue:         "HYPERLIQUID",
		VenueTargetEnvironment: "TESTNET",
		ExchangeGatewayMode:    "hyperliquid-dry-run",
		VenuePublicInfoURL:     "http://127.0.0.1:1/info",
		VenuePublicTimeout:     20 * time.Millisecond,
	})
	got := p.VenuePublicStatus(context.Background())
	if got.Connected || got.Status != "BLOCKED" || got.Error == "" {
		t.Fatalf("probe should fail closed: %+v", got)
	}
}
