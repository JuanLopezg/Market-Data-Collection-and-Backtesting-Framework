package alertack

import "testing"

func TestAppendReadAndLifecycleBinding(t *testing.T) {
	dir := t.TempDir()
	store := New(dir)
	event := Event{
		EventID: "ack-1", RecordedAt: "2026-09-26T20:00:00Z", Actor: "operator / OPERATOR",
		AlertID: "derived-nats-warn", LifecycleEventID: "alert-event-7", Severity: "WARN",
		Service: "NATS", EventType: "SOURCE_DEGRADED", Title: "NATS source is degraded",
		Comment: "investigating", CorrelationID: "ack-1",
	}
	if err := store.Append(event); err != nil {
		t.Fatal(err)
	}
	s := ReadSnapshot(dir, 20)
	if !s.Available || s.Count != 1 || len(s.Events) != 1 {
		t.Fatalf("unexpected snapshot: %+v", s)
	}
	got, ok := s.LatestByLifecycle["alert-event-7"]
	if !ok || got.AlertID != event.AlertID || got.Comment != "investigating" {
		t.Fatalf("ack lifecycle binding missing: %+v", s.LatestByLifecycle)
	}
}

func TestRepeatedAcknowledgementIsAppendOnly(t *testing.T) {
	dir := t.TempDir()
	store := New(dir)
	for _, id := range []string{"ack-1", "ack-2"} {
		if err := store.Append(Event{EventID: id, Actor: "operator / OPERATOR", AlertID: "a", LifecycleEventID: "life-1", CorrelationID: id}); err != nil {
			t.Fatal(err)
		}
	}
	s := ReadSnapshot(dir, 20)
	if !s.Available || s.Count != 2 || s.LatestByLifecycle["life-1"].EventID != "ack-2" {
		t.Fatalf("append-only replay failed: %+v", s)
	}
}

func TestUnconfiguredAndOversizedCommentFailClosed(t *testing.T) {
	if err := New("").Append(Event{EventID: "a", Actor: "x", AlertID: "a", LifecycleEventID: "l", CorrelationID: "c"}); err == nil {
		t.Fatal("unconfigured store accepted acknowledgement")
	}
	comment := make([]byte, 501)
	for i := range comment {
		comment[i] = 'x'
	}
	if err := New(t.TempDir()).Append(Event{EventID: "a", Actor: "x", AlertID: "a", LifecycleEventID: "l", CorrelationID: "c", Comment: string(comment)}); err == nil {
		t.Fatal("oversized comment accepted")
	}
}
