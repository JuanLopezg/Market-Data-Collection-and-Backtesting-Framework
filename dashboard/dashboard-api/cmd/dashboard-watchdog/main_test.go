package main

import (
	"testing"
	"time"
)

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

func TestNextSweepAtAvoidsTheTopOfTheMinute(t *testing.T) {
	base := time.Date(2026, 10, 21, 7, 0, 0, 0, time.UTC)
	cases := map[time.Duration]time.Time{
		0:                      base.Add(7 * time.Second),
		7 * time.Second:        base.Add(22 * time.Second),
		8 * time.Second:        base.Add(22 * time.Second),
		59*time.Second + 999e6: base.Add(67 * time.Second),
	}
	for offset, want := range cases {
		if got := nextSweepAt(base.Add(offset), 15*time.Second); !got.Equal(want) {
			t.Errorf("nextSweepAt(+%s) = %s, want %s", offset, got.Format("15:04:05"), want.Format("15:04:05"))
		}
	}
}
