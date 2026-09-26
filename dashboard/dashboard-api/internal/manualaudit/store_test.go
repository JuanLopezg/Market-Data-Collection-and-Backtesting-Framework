package manualaudit

import (
	"path/filepath"
	"testing"
)

func TestAppendAndReadRecent(t *testing.T) {
	dir := t.TempDir()
	store := New(dir)
	for _, id := range []string{"e1", "e2", "e3"} {
		err := store.Append(Event{
			EventID: id, RecordedAt: "2026-09-26T16:00:00Z", Actor: "operator / OPERATOR",
			Action: "MANUAL_ROUTE_ADMISSION", Target: "portfolio", Result: "REJECTED",
			RequestHash: "sha256:test", CorrelationID: "corr-" + id, ContractVersion: StoreVersion,
			Submitted: false, Blockers: []string{"PRIVATE_AUTH_DEFERRED"}, Detail: "blocked safely",
		})
		if err != nil {
			t.Fatalf("Append(%s): %v", id, err)
		}
	}
	s := ReadRecent(dir, 2)
	if !s.Available || s.Count != 2 || len(s.Events) != 2 {
		t.Fatalf("unexpected snapshot: %+v", s)
	}
	if s.Events[0].EventID != "e3" || s.Events[1].EventID != "e2" {
		t.Fatalf("unexpected order: %+v", s.Events)
	}
	if _, err := filepath.Abs(store.Dir()); err != nil {
		t.Fatalf("bad store dir: %v", err)
	}
}

func TestReadRecentEmptyDirectoryIsAvailable(t *testing.T) {
	s := ReadRecent(t.TempDir(), 20)
	if !s.Available || len(s.Events) != 0 {
		t.Fatalf("empty store should be available: %+v", s)
	}
}

func TestUnconfiguredStoreFailsClosed(t *testing.T) {
	if err := New("").Append(Event{EventID: "x", CorrelationID: "c", Actor: "a"}); err == nil {
		t.Fatal("unconfigured store unexpectedly accepted an event")
	}
	if s := ReadRecent("", 10); s.Available {
		t.Fatalf("unconfigured store unexpectedly available: %+v", s)
	}
}
