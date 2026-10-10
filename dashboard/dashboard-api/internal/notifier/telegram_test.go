package notifier

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"control-dashboard-api/internal/alertstore"
)

const testTelegramToken = "123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdef"
const testTelegramChatID = "-1001234567890"

func testNotification() Notification {
	return Notification{
		Version:          NotificationVersion,
		NotificationID:   "notify:event-1",
		SourceEventID:    "event-1",
		SourceRecordedAt: "2026-09-27T10:00:00Z",
		Transition:       "OPENED",
		AlertID:          "alert-1",
		Severity:         "CRITICAL",
		Service:          "Reconciliation",
		EventType:        "POSITION_DIVERGENCE",
		Title:            "Position divergence",
		Detail:           "local and venue position differ",
		CorrelationID:    "corr-1",
		LinkedContext:    "BTC",
		Reason:           "OPENED",
	}
}

func TestTelegramSinkSendMessageAndDurableReceiptDedup(t *testing.T) {
	hits := 0
	var got telegramSendMessageRequest
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits++
		if r.Method != http.MethodPost {
			t.Fatalf("method=%s", r.Method)
		}
		if r.URL.Path != "/bot"+testTelegramToken+"/sendMessage" {
			t.Fatalf("path=%q", r.URL.Path)
		}
		if err := json.NewDecoder(r.Body).Decode(&got); err != nil {
			t.Fatalf("decode request: %v", err)
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true,"result":{"message_id":77}}`))
	}))
	defer server.Close()

	receiptPath := filepath.Join(t.TempDir(), "telegram-receipts.jsonl")
	sink, err := newTelegramSink(testTelegramToken, testTelegramChatID, receiptPath, server.URL, server.Client())
	if err != nil {
		t.Fatal(err)
	}
	n := testNotification()
	first, err := sink.Deliver(context.Background(), n)
	if err != nil || first.AlreadyDelivered {
		t.Fatalf("first delivery=%+v err=%v", first, err)
	}
	if got.ChatID != testTelegramChatID || got.Text == "" || !got.LinkPreviewOptions.IsDisabled {
		t.Fatalf("unexpected Telegram payload: %+v", got)
	}
	if strings.Contains(got.Text, testTelegramChatID) || strings.Contains(got.Text, n.NotificationID) {
		t.Fatalf("message exposed credentials or technical identity: %q", got.Text)
	}

	second, err := sink.Deliver(context.Background(), n)
	if err != nil || !second.AlreadyDelivered {
		t.Fatalf("second delivery=%+v err=%v", second, err)
	}
	if hits != 1 {
		t.Fatalf("receipt dedup did not prevent duplicate remote send: hits=%d", hits)
	}
	exists, err := telegramReceiptContains(receiptPath, n.NotificationID)
	if err != nil || !exists {
		t.Fatalf("durable receipt missing: exists=%v err=%v", exists, err)
	}
}

func TestTelegramMessageIsPlainBoundedAndOmitsTechnicalIdentity(t *testing.T) {
	n := testNotification()
	n.Title = "unsafe *markdown* <html>"
	n.Detail = strings.Repeat("x", 10000)
	text := telegramMessage(n)
	if utf8.RuneCountInString(text) > maxTelegramTextRunes {
		t.Fatalf("Telegram message too long: %d", utf8.RuneCountInString(text))
	}
	if strings.Contains(text, n.NotificationID) || !strings.Contains(text, "unsafe *markdown* <html>") {
		t.Fatalf("message formatting exposed identity or lost plain text: %q", text[:minInt(len(text), 500)])
	}
}

func TestTelegramReadableAccountsSeverityAndResolution(t *testing.T) {
	n := testNotification()
	n.Account = "Kraken"
	text := telegramMessage(n)
	if !strings.HasPrefix(text, "[URGENT] Account: Kraken\nPosition divergence") {
		t.Fatalf("unexpected compact message: %q", text)
	}
	for _, technical := range []string{n.AlertID, n.NotificationID, "event=", "source_time=", "correlation="} {
		if strings.Contains(text, technical) {
			t.Fatalf("technical field escaped into message: %s", technical)
		}
	}
	n.Transition = "RESOLVED"
	if !strings.HasPrefix(telegramMessage(n), "[INFO] Account: Kraken\nResolved: ") {
		t.Fatal("resolution remains labelled urgent")
	}
	n.Account = ""
	n.Transition = "OPENED"
	if !strings.HasPrefix(telegramMessage(n), "[URGENT] System: Reconciliation") {
		t.Fatal("system alert is mislabelled as a trading account")
	}
}

func TestTelegramSinkRedactsSecretsFromAPIErrors(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusTooManyRequests)
		_, _ = w.Write([]byte(`{"ok":false,"error_code":429,"description":"token ` + testTelegramToken + ` chat ` + testTelegramChatID + ` blocked","parameters":{"retry_after":9}}`))
	}))
	defer server.Close()

	sink, err := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(t.TempDir(), "receipts.jsonl"), server.URL, server.Client())
	if err != nil {
		t.Fatal(err)
	}
	_, err = sink.Deliver(context.Background(), testNotification())
	if err == nil {
		t.Fatal("Telegram API rejection unexpectedly succeeded")
	}
	msg := err.Error()
	if strings.Contains(msg, testTelegramToken) || strings.Contains(msg, testTelegramChatID) {
		t.Fatalf("Telegram error leaked a secret: %q", msg)
	}
	if !strings.Contains(msg, "429") || !strings.Contains(msg, "retry_after=9s") {
		t.Fatalf("Telegram error lost useful bounded metadata: %q", msg)
	}
}

type failingRoundTripper struct{ err error }

func (f failingRoundTripper) RoundTrip(*http.Request) (*http.Response, error) { return nil, f.err }

func TestTelegramSinkNeverReturnsTransportURLWithToken(t *testing.T) {
	client := &http.Client{Transport: failingRoundTripper{err: errors.New("POST https://api.telegram.org/bot" + testTelegramToken + "/sendMessage failed")}}
	sink, err := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(t.TempDir(), "receipts.jsonl"), telegramAPIBaseURL, client)
	if err != nil {
		t.Fatal(err)
	}
	_, err = sink.Deliver(context.Background(), testNotification())
	if err == nil {
		t.Fatal("transport failure unexpectedly succeeded")
	}
	if strings.Contains(err.Error(), testTelegramToken) || strings.Contains(err.Error(), testTelegramChatID) {
		t.Fatalf("transport error leaked Telegram secrets: %q", err)
	}
}

func TestTelegramSinkTimeoutIsBounded(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(200 * time.Millisecond)
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true,"result":{"message_id":88}}`))
	}))
	defer server.Close()

	client := server.Client()
	client.Timeout = 30 * time.Millisecond
	sink, err := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(t.TempDir(), "receipts.jsonl"), server.URL, client)
	if err != nil {
		t.Fatal(err)
	}
	_, err = sink.Deliver(context.Background(), testNotification())
	if err == nil || !strings.Contains(strings.ToLower(err.Error()), "timed out") {
		t.Fatalf("expected sanitized timeout, got %v", err)
	}
}

func TestTelegramCredentialsValidation(t *testing.T) {
	if _, err := NewTelegramSink("", testTelegramChatID, filepath.Join(t.TempDir(), "r"), 5*time.Second); err == nil {
		t.Fatal("empty token accepted")
	}
	if _, err := NewTelegramSink("123:bad/token", testTelegramChatID, filepath.Join(t.TempDir(), "r"), 5*time.Second); err == nil {
		t.Fatal("unsafe token accepted")
	}
	if _, err := NewTelegramSink(testTelegramToken, "bad\nchat", filepath.Join(t.TempDir(), "r"), 5*time.Second); err == nil {
		t.Fatal("unsafe chat id accepted")
	}
}

func TestTelegramRateLimitDefersAllAlertsWithoutConsumingEvents(t *testing.T) {
	requests := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests++
		w.Header().Set("Content-Type", "application/json")
		if requests == 1 {
			w.WriteHeader(http.StatusTooManyRequests)
			fmt.Fprint(w, `{"ok":false,"error_code":429,"parameters":{"retry_after":9}}`)
			return
		}
		fmt.Fprint(w, `{"ok":true,"result":{"message_id":77}}`)
	}))
	defer server.Close()
	dir := t.TempDir()
	sink, _ := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(dir, "receipts.jsonl"), server.URL, server.Client())
	now := time.Date(2026, 10, 8, 12, 0, 0, 0, time.UTC)
	sink.now = func() time.Time { return now }
	store, _ := OpenStore(filepath.Join(dir, "state"))
	store.MarkBootstrapComplete(now)
	engine, _ := NewEngine(store, sink, "WARN", time.Minute)
	event := lifecycle("rate-limited", now.Format(time.RFC3339), "OPENED", "alert", "WARN", "fixture")
	if _, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{event}); err == nil || store.IsProcessed(event.EventID) {
		t.Fatal("Failed delivery consumed the source event")
	}
	now = now.Add(8 * time.Second)
	other := testNotification()
	if _, err := sink.Deliver(context.Background(), other); err == nil || requests != 1 {
		t.Fatal("Rate limit did not defer other alerts")
	}
	now = now.Add(time.Second)
	if _, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{event}); err != nil || !store.IsProcessed(event.EventID) || requests != 2 {
		t.Fatalf("Retry did not recover: %v requests=%d", err, requests)
	}
	// Recreate both sink and engine: durable receipts cover a lost decision checkpoint.
	restarted, _ := newTelegramSink(testTelegramToken, testTelegramChatID, filepath.Join(dir, "receipts.jsonl"), server.URL, server.Client())
	freshStore, _ := OpenStore(filepath.Join(dir, "recovered-state"))
	freshStore.MarkBootstrapComplete(now)
	recovered, _ := NewEngine(freshStore, restarted, "WARN", time.Minute)
	if _, err := recovered.Process(context.Background(), []alertstore.LifecycleEvent{event}); err != nil || requests != 2 || !freshStore.IsProcessed(event.EventID) {
		t.Fatal("Durable Telegram receipt did not prevent crash-window resend", err)
	}
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
