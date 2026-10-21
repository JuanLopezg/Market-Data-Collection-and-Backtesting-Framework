package main

import "testing"

func TestConfiguredWatchdogValidatesPostgresTimeouts(t *testing.T) {
	t.Setenv("DASHBOARD_RUNTIME_MODE", "PAPER")
	t.Setenv("DASHBOARD_EXECUTION_VENUE", "SIMULATED")
	t.Setenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "PAPER")
	t.Setenv("DASHBOARD_EXCHANGE_GATEWAY_MODE", "backend")
	for _, name := range []string{"DASHBOARD_POSTGRES_QUERY_TIMEOUT", "DASHBOARD_POSTGRES_CONNECT_TIMEOUT"} {
		t.Run(name, func(t *testing.T) {
			for _, value := range []string{"invalid", "0s", "-1s"} {
				t.Setenv(name, value)
				if _, err := configuredRealProvider(); err == nil {
					t.Fatalf("accepted %s=%s", name, value)
				}
			}
			t.Setenv(name, "11s")
			if _, err := configuredRealProvider(); err != nil {
				t.Fatal(err)
			}
		})
	}
}
