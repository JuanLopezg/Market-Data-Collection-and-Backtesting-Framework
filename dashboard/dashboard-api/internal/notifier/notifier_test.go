package notifier

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"control-dashboard-api/internal/alertstore"
)

func lifecycle(id, at, transition, alertID, severity, detail string) alertstore.LifecycleEvent {
	status := "ACTIVE"
	if transition == "RESOLVED" {
		status = "RESOLVED"
	}
	return alertstore.LifecycleEvent{
		EventID:    id,
		RecordedAt: at,
		Transition: transition,
		Alert: alertstore.Alert{
			ID:        alertID,
			Status:    status,
			Severity:  severity,
			Service:   "Reconciliation",
			EventType: "TEST_ALERT",
			Title:     "test alert",
			Detail:    detail,
		},
	}
}

func TestBootstrapOnlyNotifiesCurrentlyActiveLatestLifecycle(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenStore(filepath.Join(dir, "state"))
	if err != nil {
		t.Fatal(err)
	}
	sink, err := NewTestFileSink(filepath.Join(dir, "sink", "events.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	engine, err := NewEngine(store, sink, "WARN", 5*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	engine.now = func() time.Time { return time.Date(2026, 9, 27, 10, 0, 0, 0, time.UTC) }
	events := []alertstore.LifecycleEvent{
		lifecycle("open-old", "2026-09-27T08:00:00Z", "OPENED", "resolved", "WARN", "old"),
		lifecycle("resolved-old", "2026-09-27T08:01:00Z", "RESOLVED", "resolved", "WARN", "old"),
		lifecycle("open-active", "2026-09-27T09:00:00Z", "OPENED", "active", "WARN", "one"),
		lifecycle("update-active", "2026-09-27T09:01:00Z", "UPDATED", "active", "CRITICAL", "two"),
	}
	stats, err := engine.Process(context.Background(), events)
	if err != nil {
		t.Fatal(err)
	}
	if !stats.Bootstrap || stats.Delivered != 1 || stats.Suppressed != 3 || !store.BootstrapComplete() {
		t.Fatalf("unexpected bootstrap stats: %+v", stats)
	}
	body, err := os.ReadFile(filepath.Join(dir, "sink", "events.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var got Notification
	if err := json.Unmarshal(body[:len(body)-1], &got); err != nil {
		t.Fatal(err)
	}
	if got.SourceEventID != "update-active" || got.Reason != "BOOTSTRAP_ACTIVE" || got.Severity != "CRITICAL" {
		t.Fatalf("unexpected bootstrap notification: %+v", got)
	}
}

func TestCooldownEscalationResolutionAndRestartDedup(t *testing.T) {
	dir := t.TempDir()
	stateDir := filepath.Join(dir, "state")
	sinkPath := filepath.Join(dir, "sink", "events.jsonl")
	store, err := OpenStore(stateDir)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.MarkBootstrapComplete(time.Date(2026, 9, 27, 8, 0, 0, 0, time.UTC)); err != nil {
		t.Fatal(err)
	}
	sink, _ := NewTestFileSink(sinkPath)
	engine, _ := NewEngine(store, sink, "WARN", 5*time.Minute)
	engine.now = func() time.Time { return time.Date(2026, 9, 27, 10, 0, 0, 0, time.UTC) }
	events := []alertstore.LifecycleEvent{
		lifecycle("e1", "2026-09-27T09:00:00Z", "OPENED", "a", "WARN", "one"),
		lifecycle("e2", "2026-09-27T09:01:00Z", "UPDATED", "a", "WARN", "two"),
		lifecycle("e3", "2026-09-27T09:02:00Z", "UPDATED", "a", "CRITICAL", "three"),
		lifecycle("e4", "2026-09-27T09:03:00Z", "UPDATED", "a", "CRITICAL", "four"),
		lifecycle("e5", "2026-09-27T09:08:00Z", "UPDATED", "a", "CRITICAL", "five"),
		lifecycle("e6", "2026-09-27T09:09:00Z", "RESOLVED", "a", "CRITICAL", "done"),
	}
	stats, err := engine.Process(context.Background(), events)
	if err != nil {
		t.Fatal(err)
	}
	if stats.Delivered != 4 || stats.Suppressed != 2 {
		t.Fatalf("unexpected delivery/suppression counts: %+v", stats)
	}

	reopened, err := OpenStore(stateDir)
	if err != nil {
		t.Fatal(err)
	}
	sink2, _ := NewTestFileSink(sinkPath)
	engine2, _ := NewEngine(reopened, sink2, "WARN", 5*time.Minute)
	stats2, err := engine2.Process(context.Background(), events)
	if err != nil {
		t.Fatal(err)
	}
	if stats2.Processed != 0 || stats2.Delivered != 0 || stats2.Suppressed != 0 || stats2.Bootstrap {
		t.Fatalf("restart replay duplicated decisions: %+v", stats2)
	}
}

func TestTestFileSinkIsIdempotentByNotificationID(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sink.jsonl")
	sink, err := NewTestFileSink(path)
	if err != nil {
		t.Fatal(err)
	}
	n := Notification{NotificationID: "n1", SourceEventID: "e1", AlertID: "a"}
	first, err := sink.Deliver(context.Background(), n)
	if err != nil || first.AlreadyDelivered {
		t.Fatalf("unexpected first delivery: %+v %v", first, err)
	}
	second, err := sink.Deliver(context.Background(), n)
	if err != nil || !second.AlreadyDelivered {
		t.Fatalf("expected idempotent second delivery: %+v %v", second, err)
	}
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	lines := 0
	for _, b := range body {
		if b == '\n' {
			lines++
		}
	}
	if lines != 1 {
		t.Fatalf("expected one persisted sink event, got %d", lines)
	}
}

func TestBelowMinimumSeverityIsSuppressed(t *testing.T) {
	dir := t.TempDir()
	store, _ := OpenStore(filepath.Join(dir, "state"))
	_ = store.MarkBootstrapComplete(time.Now())
	sink, _ := NewTestFileSink(filepath.Join(dir, "sink.jsonl"))
	engine, _ := NewEngine(store, sink, "WARN", time.Minute)
	stats, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{
		lifecycle("info", "2026-09-27T09:00:00Z", "OPENED", "i", "INFO", "info"),
	})
	if err != nil {
		t.Fatal(err)
	}
	if stats.Delivered != 0 || stats.Suppressed != 1 {
		t.Fatalf("INFO was not suppressed: %+v", stats)
	}
	if _, err := os.Stat(filepath.Join(dir, "sink.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("suppressed INFO unexpectedly created sink: %v", err)
	}
}

func TestCrashWindowSinkAlreadyDeliveredDecisionMissingRecoversWithoutDuplicate(t *testing.T) {
	dir := t.TempDir()
	stateDir := filepath.Join(dir, "state")
	sinkPath := filepath.Join(dir, "sink.jsonl")
	store, err := OpenStore(stateDir)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.MarkBootstrapComplete(time.Now()); err != nil {
		t.Fatal(err)
	}
	sink, _ := NewTestFileSink(sinkPath)
	event := lifecycle("crash-e1", "2026-09-27T09:00:00Z", "OPENED", "crash-a", "WARN", "one")
	n := Notification{NotificationID: "notify:crash-e1", SourceEventID: event.EventID, AlertID: event.Alert.ID}
	if _, err := sink.Deliver(context.Background(), n); err != nil {
		t.Fatal(err)
	}

	engine, _ := NewEngine(store, sink, "WARN", time.Minute)
	if _, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{event}); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(sinkPath)
	if err != nil {
		t.Fatal(err)
	}
	lines := 0
	for _, b := range body {
		if b == '\n' {
			lines++
		}
	}
	if lines != 1 {
		t.Fatalf("crash-window recovery duplicated sink delivery: %d lines", lines)
	}
	if !store.IsProcessed(event.EventID) {
		t.Fatal("crash-window recovery did not persist missing decision")
	}
}
