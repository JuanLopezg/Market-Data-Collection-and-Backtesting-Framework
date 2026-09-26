package alertstore

import (
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
