package notifier

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
	"unicode/utf8"
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
	if strings.Contains(got.Text, testTelegramChatID) || !strings.Contains(got.Text, n.NotificationID) {
		t.Fatalf("message leaked chat id or omitted notification id: %q", got.Text)
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

func TestTelegramMessageIsPlainBoundedAndKeepsNotificationIdentity(t *testing.T) {
	n := testNotification()
	n.Title = "unsafe *markdown* <html>"
	n.Detail = strings.Repeat("x", 10000)
	text := telegramMessage(n)
	if utf8.RuneCountInString(text) > maxTelegramTextRunes {
		t.Fatalf("Telegram message too long: %d", utf8.RuneCountInString(text))
	}
	if !strings.Contains(text, n.NotificationID) || !strings.Contains(text, "unsafe *markdown* <html>") {
		t.Fatalf("message formatting lost identity/plain text: %q", text[:minInt(len(text), 500)])
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

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
