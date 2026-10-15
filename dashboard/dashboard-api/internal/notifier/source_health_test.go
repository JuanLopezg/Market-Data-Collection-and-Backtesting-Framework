package notifier

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func sourceNotifications(t *testing.T, path string) []Notification {
	t.Helper()
	body, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		t.Fatal(err)
	}
	var result []Notification
	for _, line := range strings.Split(strings.TrimSpace(string(body)), "\n") {
		var n Notification
		if err := json.Unmarshal([]byte(line), &n); err != nil {
			t.Fatal(err)
		}
		result = append(result, n)
	}
	return result
}

func TestSourceMonitorDurableDelayWarningRecoveryAndNewIncident(t *testing.T) {
	for _, reason := range []SourceFailure{SourceMissing, SourceStale, SourceAhead, SourceError} {
		t.Run(string(reason), func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, "sink.jsonl")
			sink, _ := NewTestFileSink(path)
			var monitor *SourceMonitor
			var store *Store
			reopen := func() {
				var err error
				store, err = OpenStore(dir)
				if err != nil {
					t.Fatal(err)
				}
				monitor, err = NewSourceMonitor(store, sink, "Kraken", 5*time.Minute)
				if err != nil {
					t.Fatal(err)
				}
			}
			reopen()
			now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
			observe := func(at time.Time, failure SourceFailure) {
				t.Helper()
				if err := monitor.Observe(context.Background(), at, failure); err != nil {
					t.Fatal(err)
				}
			}
			observe(now, reason)
			observe(now.Add(299*time.Second), reason)
			if len(sourceNotifications(t, path)) != 0 {
				t.Fatal("warning before five minutes")
			}
			reopen() // The waiting period must survive a restart, too.
			observe(now.Add(5*time.Minute), reason)
			observe(now.Add(6*time.Minute), SourceError) // Changing cause does not start another incident.
			reopen()
			observe(now.Add(7*time.Minute), reason)
			ns := sourceNotifications(t, path)
			if len(ns) != 1 || ns[0].Title != "Alert source stopped" || ns[0].Severity != "WARN" ||
				!strings.Contains(ns[0].Detail, "Kraken") || !strings.Contains(ns[0].Detail, now.Format(time.RFC3339)) ||
				!strings.Contains(ns[0].Detail, string(reason)) {
				t.Fatalf("unexpected warning: %+v", ns)
			}
			if store.BootstrapComplete() {
				t.Fatal("source warning bootstrapped producer history")
			}
			observe(now.Add(8*time.Minute), "")
			reopen()
			observe(now.Add(9*time.Minute), "")
			ns = sourceNotifications(t, path)
			if len(ns) != 2 || ns[1].Transition != "RESOLVED" {
				t.Fatalf("unexpected recovery: %+v", ns)
			}
			observe(now.Add(10*time.Minute), reason)
			observe(now.Add(15*time.Minute), reason)
			ns = sourceNotifications(t, path)
			if len(ns) != 3 || ns[0].NotificationID == ns[2].NotificationID {
				t.Fatal("new incident did not get a new identity")
			}
		})
	}
}

func TestSourceMonitorShortFailureResetsContinuousTimer(t *testing.T) {
	dir := t.TempDir()
	store, _ := OpenStore(dir)
	path := filepath.Join(dir, "sink.jsonl")
	sink, _ := NewTestFileSink(path)
	m, _ := NewSourceMonitor(store, sink, "Watchdog", 5*time.Minute)
	now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
	for _, step := range []struct {
		seconds int
		reason  SourceFailure
	}{
		{0, SourceStale}, {299, ""}, {300, SourceMissing}, {599, SourceError},
	} {
		if err := m.Observe(context.Background(), now.Add(time.Duration(step.seconds)*time.Second), step.reason); err != nil {
			t.Fatal(err)
		}
	}
	if len(sourceNotifications(t, path)) != 0 {
		t.Fatal("short incidents accumulated or sent a recovery")
	}
	if err := m.Observe(context.Background(), now.Add(600*time.Second), SourceAhead); err != nil {
		t.Fatal(err)
	}
	if len(sourceNotifications(t, path)) != 1 {
		t.Fatal("continuous failure with changing reasons did not warn")
	}
}

func TestSourceMonitorTelegramNormalChatPriorityAndReceiptCrashRecovery(t *testing.T) {
	var requests []telegramSendMessageRequest
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var payload telegramSendMessageRequest
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			t.Error(err)
		}
		requests = append(requests, payload)
		fmt.Fprintf(w, `{"ok":true,"result":{"message_id":%d}}`, len(requests))
	}))
	defer server.Close()
	dir := t.TempDir()
	newSink := func() *TelegramSink {
		sink, err := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(dir, "receipts.jsonl"), server.URL, server.Client())
		if err != nil {
			t.Fatal(err)
		}
		sink.criticalChatID = "-1009876543210"
		return sink
	}
	store, _ := OpenStore(dir)
	sink := newSink()
	m, err := NewSourceMonitor(store, sink, "Kraken", 5*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
	if err := m.Observe(context.Background(), now, SourceStale); err != nil {
		t.Fatal(err)
	}
	// Telegram acceptance and receipt succeed, then decision persistence fails.
	decisionsPath := filepath.Join(dir, decisionsFileName)
	if err := os.Mkdir(decisionsPath, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := m.Observe(context.Background(), now.Add(5*time.Minute), SourceStale); err == nil {
		t.Fatal("decision failure not surfaced")
	}
	if len(requests) != 1 {
		t.Fatalf("requests=%d", len(requests))
	}
	if err := os.Remove(decisionsPath); err != nil {
		t.Fatal(err)
	}
	store, err = OpenStore(dir)
	if err != nil {
		t.Fatal(err)
	}
	m, err = NewSourceMonitor(store, newSink(), "Kraken", 5*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Observe(context.Background(), now.Add(6*time.Minute), SourceStale); err != nil {
		t.Fatal(err)
	}
	if len(requests) != 1 {
		t.Fatal("confirmed Telegram warning resent after losing decision")
	}
	if err := m.Observe(context.Background(), now.Add(7*time.Minute), ""); err != nil {
		t.Fatal(err)
	}
	if len(requests) != 2 {
		t.Fatalf("requests=%d", len(requests))
	}
	for i, payload := range requests {
		if payload.ChatID != testTelegramChatID {
			t.Fatal("source WARN/recovery routed to critical chat")
		}
		if payload.DisableNotification != (i == 1) {
			t.Fatalf("wrong sound policy: %+v", payload)
		}
		marker := "🟡"
		if i == 1 {
			marker = "🟢"
		}
		if !strings.Contains(payload.Text, marker) || !strings.Contains(payload.Text, "Kraken") {
			t.Fatalf("unexpected message: %s", payload.Text)
		}
	}
}

func TestSourceMonitorDeliveryFailureRetriesAndRetainsRecovery(t *testing.T) {
	dir := t.TempDir()
	store, _ := OpenStore(dir)
	path := filepath.Join(dir, "sink.jsonl")
	sink, _ := NewTestFileSink(path)
	m, _ := NewSourceMonitor(store, sink, "Watchdog", time.Second)
	now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
	if err := m.Observe(context.Background(), now, SourceMissing); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := m.Observe(ctx, now.Add(time.Second), SourceMissing); err == nil {
		t.Fatal("cancelled delivery accepted")
	}
	if err := m.Observe(ctx, now.Add(2*time.Second), ""); err == nil {
		t.Fatal("cancelled recovery accepted")
	}
	store, _ = OpenStore(dir)
	m, err := NewSourceMonitor(store, sink, "Watchdog", time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err := m.Observe(context.Background(), now.Add(3*time.Second), ""); err != nil {
		t.Fatal(err)
	}
	ns := sourceNotifications(t, path)
	if len(ns) != 2 || ns[0].Transition != "OPENED" || ns[1].Transition != "RESOLVED" {
		t.Fatalf("lost pending transitions: %+v", ns)
	}
}

func TestSourceMonitorRejectsUnsafeOrCorruptState(t *testing.T) {
	dir := t.TempDir()
	store, _ := OpenStore(dir)
	sink, _ := NewTestFileSink(filepath.Join(dir, "sink.jsonl"))
	if _, err := NewSourceMonitor(store, sink, "/secrets/private", time.Minute); err == nil {
		t.Fatal("path used as display name")
	}
	if err := os.WriteFile(filepath.Join(dir, "source-health.json"), []byte("{"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewSourceMonitor(store, sink, "Kraken", time.Minute); err == nil {
		t.Fatal("corrupt incident silently discarded")
	}
}
