package alertstore

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func TestStorePersistsOpenUpdateResolveLifecycle(t *testing.T) {
	dir := t.TempDir()
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	t0 := time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC)
	a := Alert{ID: "a1", Status: "ACTIVE", Severity: "WARN", Service: "MarketData", EventType: "STALE_DATA", Title: "stale", Detail: "one"}
	hb, err := store.Apply(t0, []Alert{a})
	if err != nil {
		t.Fatal(err)
	}
	if hb.EventCount != 1 || hb.ActiveWarnings != 1 {
		t.Fatalf("unexpected heartbeat %+v", hb)
	}

	a.Detail = "two"
	hb, err = store.Apply(t0.Add(time.Second), []Alert{a})
	if err != nil {
		t.Fatal(err)
	}
	if hb.EventCount != 2 {
		t.Fatalf("expected update event, got %+v", hb)
	}

	hb, err = store.Apply(t0.Add(2*time.Second), nil)
	if err != nil {
		t.Fatal(err)
	}
	if hb.EventCount != 3 || hb.ActiveCount != 0 {
		t.Fatalf("expected resolve event, got %+v", hb)
	}

	snapshot := ReadSnapshot(dir, 10)
	if !snapshot.Available || len(snapshot.Events) != 3 {
		t.Fatalf("unexpected snapshot %+v", snapshot)
	}
	if snapshot.Events[0].Transition != "RESOLVED" || snapshot.Events[1].Transition != "UPDATED" || snapshot.Events[2].Transition != "OPENED" {
		t.Fatalf("unexpected transitions %+v", snapshot.Events)
	}
}

func TestStoreRestartReplaysActiveStateWithoutDuplicateOpen(t *testing.T) {
	dir := t.TempDir()
	t0 := time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC)
	alert := Alert{ID: "stable", Status: "ACTIVE", Severity: "CRITICAL", Service: "Reconciliation", EventType: "RECONCILIATION_BLOCKED", Title: "blocked", Detail: "mismatch"}
	first, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := first.Apply(t0, []Alert{alert}); err != nil {
		t.Fatal(err)
	}

	second, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	hb, err := second.Apply(t0.Add(time.Minute), []Alert{alert})
	if err != nil {
		t.Fatal(err)
	}
	if hb.EventCount != 1 || hb.ActiveCritical != 1 {
		t.Fatalf("restart duplicated lifecycle event: %+v", hb)
	}
}

func TestRecordFailureDoesNotResolveActiveAlerts(t *testing.T) {
	dir := t.TempDir()
	t0 := time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC)
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	alert := Alert{ID: "a", Status: "ACTIVE", Severity: "WARN", Service: "NATS", EventType: "SOURCE_DEGRADED", Title: "warn", Detail: "lag"}
	if _, err := store.Apply(t0, []Alert{alert}); err != nil {
		t.Fatal(err)
	}
	if err := store.RecordFailure(t0.Add(time.Second), assertErr("temporary read failure")); err != nil {
		t.Fatal(err)
	}

	reopened, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	hb, err := reopened.Apply(t0.Add(2*time.Second), []Alert{alert})
	if err != nil {
		t.Fatal(err)
	}
	if hb.EventCount != 1 {
		t.Fatalf("failure incorrectly changed active lifecycle: %+v", hb)
	}
}

type assertErr string

func (e assertErr) Error() string { return string(e) }

func TestReadSnapshotReconstructsActiveBeyondRecentWindow(t *testing.T) {
	dir := t.TempDir()
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	t0 := time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC)
	longLived := Alert{ID: "long-lived", Status: "ACTIVE", Severity: "WARN", Service: "NATS", EventType: "SOURCE_DEGRADED", Title: "degraded", Detail: "stable"}
	if _, err := store.Apply(t0, []Alert{longLived}); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 30; i++ {
		temp := Alert{ID: "temp", Status: "ACTIVE", Severity: "INFO", Service: "Test", EventType: "CHURN", Title: "churn", Detail: fmt.Sprintf("%d", i)}
		if _, err := store.Apply(t0.Add(time.Duration(i+1)*time.Second), []Alert{longLived, temp}); err != nil {
			t.Fatal(err)
		}
		if _, err := store.Apply(t0.Add(time.Duration(i+1)*time.Second+500*time.Millisecond), []Alert{longLived}); err != nil {
			t.Fatal(err)
		}
	}
	snapshot := ReadSnapshot(dir, 5)
	if !snapshot.Available {
		t.Fatalf("snapshot unavailable: %+v", snapshot)
	}
	active, ok := snapshot.Active["long-lived"]
	if !ok || active.Alert.ID != "long-lived" || active.LifecycleEventID == "" {
		t.Fatalf("long-lived active alert was lost outside recent window: %+v", snapshot.Active)
	}
}

func TestReadLifecycleEventsReturnsAppendOrderAndFailsClosedOnBound(t *testing.T) {
	dir := t.TempDir()
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	t0 := time.Date(2026, 9, 27, 8, 0, 0, 0, time.UTC)
	alert := Alert{ID: "a", Status: "ACTIVE", Severity: "WARN", Service: "NATS", EventType: "SOURCE_DEGRADED", Title: "warn", Detail: "one"}
	if _, err := store.Apply(t0, []Alert{alert}); err != nil {
		t.Fatal(err)
	}
	alert.Detail = "two"
	if _, err := store.Apply(t0.Add(time.Second), []Alert{alert}); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Apply(t0.Add(2*time.Second), nil); err != nil {
		t.Fatal(err)
	}

	events, err := ReadLifecycleEvents(dir, 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 3 || events[0].Transition != "OPENED" || events[1].Transition != "UPDATED" || events[2].Transition != "RESOLVED" {
		t.Fatalf("unexpected lifecycle order: %+v", events)
	}
	if _, err := ReadLifecycleEvents(dir, 1); err == nil {
		t.Fatal("expected bounded lifecycle read to fail closed")
	}
}

func TestReadLifecycleEventsAcceptsLegacyLargeLifecycleRecordWithinBound(t *testing.T) {
	dir := t.TempDir()
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	t0 := time.Date(2026, 9, 27, 9, 0, 0, 0, time.UTC)
	// Step43 never imposed a 64 KiB lifecycle-record contract. Real durable
	// alerts can therefore legitimately contain details above Scanner's default
	// token ceiling. Step46B must replay them while still honoring maxBytes.
	alert := Alert{
		ID:        "large-detail",
		Status:    "ACTIVE",
		Severity:  "WARN",
		Service:   "SymbolRegistry",
		EventType: "SOURCE_DEGRADED",
		Title:     "large but valid lifecycle detail",
		Detail:    strings.Repeat("x", 128*1024),
	}
	if _, err := store.Apply(t0, []Alert{alert}); err != nil {
		t.Fatal(err)
	}

	events, err := ReadLifecycleEvents(dir, 2<<20)
	if err != nil {
		t.Fatalf("large valid lifecycle record was rejected: %v", err)
	}
	if len(events) != 1 || events[0].Alert.ID != alert.ID || len(events[0].Alert.Detail) != len(alert.Detail) {
		t.Fatalf("large lifecycle event changed during replay: %+v", events)
	}
}

func TestReadHeartbeatRequiresInitializedStep43Store(t *testing.T) {
	dir := t.TempDir()
	if _, err := ReadHeartbeat(dir); err == nil {
		t.Fatal("empty directory unexpectedly passed heartbeat validation")
	}
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.Apply(time.Date(2026, 9, 27, 8, 0, 0, 0, time.UTC), nil); err != nil {
		t.Fatal(err)
	}
	hb, err := ReadHeartbeat(dir)
	if err != nil {
		t.Fatal(err)
	}
	if hb.Version != "step43-v1" || hb.LastSuccessAt == "" {
		t.Fatalf("unexpected heartbeat: %+v", hb)
	}
}
