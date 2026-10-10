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

func TestProcessAndClockEvidenceDoesNotClaimTradingReady(t *testing.T) {
	now := time.Now().UTC()
	path := filepath.Join(t.TempDir(), "host.json")
	running := true
	snapshot := hostTelemetry{SchemaVersion: 1, Project: "algotrading-paper", ObservedAt: now.Format(time.RFC3339Nano),
		VPS:        &realVPS{State: "HEALTHY", ClockObserved: true, ClockSynced: true, ClockOffsetLabel: "Host systemd NTP synchronized; offset not measured"},
		Containers: []realContainer{{Name: "strategy", State: "RUNNING", Health: "HEALTHY"}},
		Processes:  []hostProcess{{Service: "strategy", ProcessRunning: &running, ProcessState: "RUNNING", Detail: "Executable observed"}}}
	write := func() {
		t.Helper()
		data, _ := json.Marshal(snapshot)
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	p := NewReal(RealConfig{HostMetricsFile: path, HostMetricsProject: snapshot.Project, RuntimeMode: "PAPER"})
	apply := func() realInfrastructure {
		write()
		result := realInfrastructure{Dependencies: []realReadinessDependency{{Component: "Trading service liveness", State: "UNKNOWN"}, {Component: "Clock sync", State: "UNKNOWN"}}}
		p.applyHostTelemetry(&result, now)
		return result
	}
	result := apply()
	if len(result.Services) != 1 || result.Services[0].Ready || result.Services[0].ProcessRunning == nil || !*result.Services[0].ProcessRunning || result.Dependencies[1].State != "HEALTHY" {
		t.Fatalf("Process/clock evidence incorrectly applied: %+v", result)
	}
	running = false
	snapshot.Processes[0].ProcessState = "MISSING"
	if result = apply(); result.Dependencies[0].State != "CRITICAL" {
		t.Fatal("Running container hid absent executable")
	}
	snapshot.Processes = nil
	if result = apply(); result.Dependencies[0].State != "UNKNOWN" || len(result.Services) != 0 {
		t.Fatal("Legacy collector fabricated process evidence")
	}
	snapshot.VPS.ClockSynced = false
	if result = apply(); result.Dependencies[1].State != "CRITICAL" {
		t.Fatal("Unsynchronized clock hidden")
	}
	snapshot.VPS.ClockObserved = false
	if result = apply(); result.Dependencies[1].State != "UNKNOWN" {
		t.Fatal("Unavailable clock guessed")
	}
	snapshot.VPS.ClockSynced = true
	write()
	if _, err := readHostTelemetry(path, snapshot.Project, now); err == nil {
		t.Fatal("Unobserved clock marked synchronized")
	}
}
