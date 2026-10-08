package provider

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestHostTelemetryFreshnessAndIdentity(t *testing.T) {
	now := time.Now().UTC()
	path := filepath.Join(t.TempDir(), "host.json")
	snapshot := hostTelemetry{SchemaVersion: 1, Project: "algotrading-paper", ObservedAt: now.Format(time.RFC3339Nano),
		VPS:        &realVPS{State: "HEALTHY", CPUPct: 12, RAMPct: 30, DiskPct: 25},
		Containers: []realContainer{{Name: "strategy", State: "RUNNING", Health: "WARN", CPUPct: -1, RAMMB: -1}}}
	write := func() {
		t.Helper()
		data, _ := json.Marshal(snapshot)
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	write()
	if _, err := readHostTelemetry(path, snapshot.Project, now); err != nil {
		t.Fatal(err)
	}
	if _, err := readHostTelemetry(path, "another-project", now); err == nil {
		t.Fatal("Wrong project accepted")
	}
	if _, err := readHostTelemetry(path, snapshot.Project, now.Add(36*time.Second)); err == nil {
		t.Fatal("Stale telemetry accepted")
	}
	if _, err := readHostTelemetry(path, snapshot.Project, now.Add(-6*time.Second)); err == nil {
		t.Fatal("Future telemetry accepted")
	}
	snapshot.VPS.CPUPct = 101
	write()
	if _, err := readHostTelemetry(path, snapshot.Project, now); err == nil {
		t.Fatal("Invalid percentage accepted")
	}
	if err := os.WriteFile(path, []byte(`{"schemaVersion":1,"project":"algotrading-paper","observedAt":"`+now.Format(time.RFC3339Nano)+`","vps":{"state":"HEALTHY"}}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := readHostTelemetry(path, snapshot.Project, now); err == nil {
		t.Fatal("Missing measurements appeared as zero")
	}
}

func TestStaleHostTelemetryDoesNotClaimHealthy(t *testing.T) {
	result := realInfrastructure{VPS: realVPS{State: "UNKNOWN"}, SourceNote: "Read-only sources."}
	p := NewReal(RealConfig{HostMetricsFile: filepath.Join(t.TempDir(), "missing"), RuntimeMode: "PAPER"})
	p.applyHostTelemetry(&result, time.Now())
	if result.VPS.State != "UNKNOWN" || len(result.Containers) != 0 || result.TelemetryObservedAt != "" {
		t.Fatal("Missing telemetry falsely claimed health")
	}
}
