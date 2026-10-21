package main

import (
	"testing"
)

func TestConfiguredRealProviderRequiresTheTradingChain(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	for _, setup := range [][4]string{
		{"", "", "", ""}, // nothing configured
		{"LIVE", "HYPERLIQUID", "TESTNET", "hyperliquid-dry-run"}, // the retired testnet setup
		{"LIVE", "BINANCE", "LIVE", "venue"},                      // a venue the chain does not trade
	} {
		t.Setenv("DASHBOARD_RUNTIME_MODE", setup[0])
		t.Setenv("DASHBOARD_EXECUTION_VENUE", setup[1])
		t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", setup[2])
		t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", setup[3])
		if _, err := configuredProvider(); err == nil {
			t.Fatalf("real provider accepted a setup outside the trading chain: %v", setup)
		}
	}
}

func TestConfiguredProviderValidatesPostgresTimeouts(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_RUNTIME_MODE", "PAPER")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "SIMULATED")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "PAPER")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "backend")
	for _, name := range []string{"DASHBOARD_POSTGRES_QUERY_TIMEOUT", "DASHBOARD_POSTGRES_CONNECT_TIMEOUT"} {
		t.Run(name, func(t *testing.T) {
			for _, value := range []string{"invalid", "0s", "-1s"} {
				t.Setenv(name, value)
				if _, err := configuredProvider(); err == nil {
					t.Fatalf("accepted %s=%s", name, value)
				}
			}
			t.Setenv(name, "11s")
			if _, err := configuredProvider(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestConfiguredProviderPaperRequiresFullySimulatedBoundary(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_RUNTIME_MODE", "PAPER")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "SIMULATED")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "PAPER")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "backend")
	if _, err := configuredProvider(); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "HYPERLIQUID")
	if _, err := configuredProvider(); err == nil {
		t.Fatal("Mixed paper/private venue configuration accepted")
	}
}

func TestConfiguredProviderAcceptsOnlyTheFullKrakenLiveBoundary(t *testing.T) {
	t.Setenv("DASHBOARD_DATA_PROVIDER", "real")
	t.Setenv("DASHBOARD_RUNTIME_MODE", "LIVE")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "KRAKEN")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "LIVE")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "venue")
	if _, err := configuredProvider(); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "backend")
	if _, err := configuredProvider(); err == nil {
		t.Fatal("Kraken with the simulated backend gateway accepted")
	}
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "venue")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "PAPER")
	if _, err := configuredProvider(); err == nil {
		t.Fatal("Kraken with a PAPER target accepted")
	}
}
