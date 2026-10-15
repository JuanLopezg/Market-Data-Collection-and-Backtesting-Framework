package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/alertstore"
	"control-dashboard-api/internal/notifier"
)

func TestNotifierReadsOnlyHealthyFreshWatchdogEvidence(t *testing.T) {
	dir := t.TempDir()
	now := time.Date(2026, 10, 8, 18, 0, 0, 0, time.UTC)
	event := alertstore.LifecycleEvent{EventID: "fixture-event", Transition: "OPENED", Alert: alertstore.Alert{ID: "fixture-alert"}}
	body, _ := json.Marshal(event)
	os.WriteFile(filepath.Join(dir, "events.jsonl"), append(body, '\n'), 0600)
	for _, test := range []struct {
		name, lastSuccess, lastError string
		valid                        bool
	}{
		{"fresh", now.Format(time.RFC3339), "", true},
		{"stale", now.Add(-91 * time.Second).Format(time.RFC3339), "", false},
		{"future", now.Add(31 * time.Second).Format(time.RFC3339), "", false},
		{"missing success", "", "", false},
		{"failed sweep", now.Format(time.RFC3339), "fixture error", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			body, _ := json.Marshal(alertstore.Heartbeat{Version: "step43-v1", LastSuccessAt: test.lastSuccess, LastError: test.lastError})
			os.WriteFile(filepath.Join(dir, "status.json"), body, 0600)
			events, err := readFreshAlertEvents(dir, 1024*1024, now)
			if (err == nil) != test.valid || (test.valid && len(events) != 1) {
				t.Fatalf("Unexpected source freshness result: events=%d err=%v", len(events), err)
			}
		})
	}
}

func clearNotifierEnv(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		"DASHBOARD_ALERT_STORE_DIR", "DASHBOARD_NOTIFIER_STORE_DIR", "DASHBOARD_NOTIFIER_TEST_SINK_FILE",
		"DASHBOARD_NOTIFIER_SINK", "DASHBOARD_NOTIFIER_MIN_SEVERITY", "DASHBOARD_NOTIFIER_INTERVAL",
		"DASHBOARD_NOTIFIER_COOLDOWN", "DASHBOARD_NOTIFIER_SWEEP_TIMEOUT", "DASHBOARD_NOTIFIER_SOURCE_MAX_BYTES",
		"DASHBOARD_TELEGRAM_BOT_TOKEN", "DASHBOARD_TELEGRAM_BOT_TOKEN_FILE", "DASHBOARD_TELEGRAM_CHAT_ID",
		"DASHBOARD_TELEGRAM_CHAT_ID_FILE", "DASHBOARD_TELEGRAM_RECEIPT_FILE", "DASHBOARD_TELEGRAM_REQUEST_TIMEOUT",
		"DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID", "DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID_FILE",
		"DASHBOARD_NOTIFIER_SOURCE_NAME", "DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS",
	} {
		t.Setenv(name, "")
	}
}

func TestLoadConfigStep46CDefaultsRemainTestFile(t *testing.T) {
	clearNotifierEnv(t)
	cfg, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.alertStoreDir != "/data/watchdog" || cfg.storeDir != "/data/notifier" || cfg.sink != "TEST_FILE" || cfg.minSeverity != "WARN" {
		t.Fatalf("unexpected defaults: %+v", cfg)
	}
	if cfg.cooldown != 5*time.Minute || cfg.interval != 5*time.Second {
		t.Fatalf("unexpected timing defaults: %+v", cfg)
	}
	if cfg.sourceName != "Watchdog" || cfg.sourceStaleWarning != 5*time.Minute {
		t.Fatal("unexpected source monitor defaults")
	}
	if cfg.telegramBotToken != "" || cfg.telegramChatID != "" {
		t.Fatal("TEST_FILE mode unexpectedly loaded Telegram credentials")
	}
}

func TestLoadConfigTelegramRequiresCredentials(t *testing.T) {
	clearNotifierEnv(t)
	t.Setenv("DASHBOARD_NOTIFIER_SINK", "TELEGRAM")
	if _, err := loadConfig(); err == nil {
		t.Fatal("TELEGRAM sink accepted without credentials")
	}
}

func TestLoadConfigTelegramAcceptsDeploymentEnvWithoutLoggingSurface(t *testing.T) {
	clearNotifierEnv(t)
	t.Setenv("DASHBOARD_NOTIFIER_SINK", "TELEGRAM")
	t.Setenv("DASHBOARD_TELEGRAM_BOT_TOKEN", "123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdef")
	t.Setenv("DASHBOARD_TELEGRAM_CHAT_ID", "-1001234567890")
	t.Setenv("DASHBOARD_TELEGRAM_REQUEST_TIMEOUT", "7s")
	t.Setenv("DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID", "-1009876543210")
	cfg, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.sink != "TELEGRAM" || cfg.telegramBotToken == "" || cfg.telegramChatID == "" || cfg.telegramCriticalChatID != "-1009876543210" || cfg.telegramRequestTimeout != 7*time.Second {
		t.Fatalf("unexpected Telegram config: sink=%s timeout=%s tokenSet=%v chatSet=%v", cfg.sink, cfg.telegramRequestTimeout, cfg.telegramBotToken != "", cfg.telegramChatID != "")
	}
}

func TestLoadConfigTelegramAcceptsSecretFiles(t *testing.T) {
	clearNotifierEnv(t)
	dir := t.TempDir()
	tokenFile := filepath.Join(dir, "token")
	chatFile := filepath.Join(dir, "chat")
	if err := os.WriteFile(tokenFile, []byte("123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdef\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(chatFile, []byte("-1001234567890\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DASHBOARD_NOTIFIER_SINK", "TELEGRAM")
	t.Setenv("DASHBOARD_TELEGRAM_BOT_TOKEN_FILE", tokenFile)
	t.Setenv("DASHBOARD_TELEGRAM_CHAT_ID_FILE", chatFile)
	cfg, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(cfg.telegramBotToken, "123456789:") || cfg.telegramChatID != "-1001234567890" {
		t.Fatal("secret-file Telegram credentials were not loaded")
	}
}

func TestLoadConfigRejectsAmbiguousSecretSources(t *testing.T) {
	clearNotifierEnv(t)
	t.Setenv("DASHBOARD_NOTIFIER_SINK", "TELEGRAM")
	t.Setenv("DASHBOARD_TELEGRAM_BOT_TOKEN", "123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdef")
	t.Setenv("DASHBOARD_TELEGRAM_BOT_TOKEN_FILE", "/tmp/token")
	t.Setenv("DASHBOARD_TELEGRAM_CHAT_ID", "-1001234567890")
	if _, err := loadConfig(); err == nil {
		t.Fatal("ambiguous Telegram token sources accepted")
	}
}

func TestLoadConfigRejectsInvalidSeverityAndBounds(t *testing.T) {
	clearNotifierEnv(t)
	t.Setenv("DASHBOARD_NOTIFIER_MIN_SEVERITY", "PANIC")
	if _, err := loadConfig(); err == nil {
		t.Fatal("invalid severity accepted")
	}
	t.Setenv("DASHBOARD_NOTIFIER_MIN_SEVERITY", "WARN")
	t.Setenv("DASHBOARD_NOTIFIER_INTERVAL", "100ms")
	if _, err := loadConfig(); err == nil {
		t.Fatal("unsafe notifier interval accepted")
	}
}

func TestLoadConfigSourceWarningSettings(t *testing.T) {
	clearNotifierEnv(t)
	t.Setenv("DASHBOARD_NOTIFIER_SOURCE_NAME", "Kraken")
	t.Setenv("DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS", "600")
	cfg, err := loadConfig()
	if err != nil || cfg.sourceName != "Kraken" || cfg.sourceStaleWarning != 10*time.Minute {
		t.Fatalf("source config: %v", err)
	}
	for _, value := range []string{"0", "-1", "300s", "86401", "secret-invalid"} {
		t.Setenv("DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS", value)
		if _, err := loadConfig(); err == nil {
			t.Fatalf("invalid seconds %q accepted", value)
		}
	}
	t.Setenv("DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS", "300")
	t.Setenv("DASHBOARD_NOTIFIER_SOURCE_NAME", "/internal/private/path")
	if _, err := loadConfig(); err == nil {
		t.Fatal("internal path accepted as source name")
	}
}

func TestSourceHeartbeatFailureCategoriesAreSafe(t *testing.T) {
	now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
	for _, tc := range []struct {
		name, success, lastError string
		reason                   notifier.SourceFailure
	}{
		{"missing", "", "", notifier.SourceMissing},
		{"malformed time", "invalid-sensitive-value", "", notifier.SourceError},
		{"stale", now.Add(-91 * time.Second).Format(time.RFC3339), "", notifier.SourceStale},
		{"ahead", now.Add(31 * time.Second).Format(time.RFC3339), "", notifier.SourceAhead},
		{"source error", now.Format(time.RFC3339), "/internal/path?token=do-not-leak", notifier.SourceError},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			body, _ := json.Marshal(alertstore.Heartbeat{Version: "step43-v1", LastSuccessAt: tc.success, LastError: tc.lastError})
			if err := os.WriteFile(filepath.Join(dir, "status.json"), body, 0o600); err != nil {
				t.Fatal(err)
			}
			_, err := readFreshAlertEvents(dir, 1<<20, now)
			var failure sourceReadError
			if !errors.As(err, &failure) || failure.reason != tc.reason {
				t.Fatalf("unexpected category: %v", err)
			}
			if strings.Contains(err.Error(), dir) || strings.Contains(err.Error(), "sensitive") || strings.Contains(err.Error(), "token") {
				t.Fatalf("source failure leaked raw content: %v", err)
			}
		})
	}
	_, err := readFreshAlertEvents(t.TempDir(), 1<<20, now)
	var failure sourceReadError
	if !errors.As(err, &failure) || failure.reason != notifier.SourceMissing {
		t.Fatalf("missing file category: %v", err)
	}
}

func TestSourceSweepBlocksProducerReplayAndPreservesBootstrapAcrossRestart(t *testing.T) {
	sourceDir, storeDir := t.TempDir(), t.TempDir()
	path := filepath.Join(t.TempDir(), "sink.jsonl")
	sink, err := notifier.NewTestFileSink(path)
	if err != nil {
		t.Fatal(err)
	}
	cfg := config{alertStoreDir: sourceDir, sourceMaxBytes: 1 << 20}
	now := time.Date(2026, 10, 15, 7, 0, 0, 0, time.UTC)
	// An active old warning and a resolved historical incident are waiting in source.
	events := []alertstore.LifecycleEvent{
		{EventID: "old-open", RecordedAt: now.Add(-time.Hour).Format(time.RFC3339), Transition: "OPENED", Alert: alertstore.Alert{ID: "old-alert", Severity: "WARN", Title: "Old active warning"}},
		{EventID: "history-open", Transition: "OPENED", Alert: alertstore.Alert{ID: "historical", Severity: "WARN"}},
		{EventID: "history-resolved", Transition: "RESOLVED", Alert: alertstore.Alert{ID: "historical", Severity: "WARN"}},
	}
	var lines []byte
	for _, e := range events {
		body, _ := json.Marshal(e)
		lines = append(lines, append(body, '\n')...)
	}
	if err := os.WriteFile(filepath.Join(sourceDir, "events.jsonl"), lines, 0o600); err != nil {
		t.Fatal(err)
	}
	var store *notifier.Store
	var monitor *notifier.SourceMonitor
	var engine *notifier.Engine
	reopen := func() {
		var err error
		store, err = notifier.OpenStore(storeDir)
		if err != nil {
			t.Fatal(err)
		}
		monitor, err = notifier.NewSourceMonitor(store, sink, "Kraken", 5*time.Minute)
		if err != nil {
			t.Fatal(err)
		}
		engine, err = notifier.NewEngine(store, sink, "WARN", 5*time.Minute)
		if err != nil {
			t.Fatal(err)
		}
	}
	readSink := func() []notifier.Notification {
		body, err := os.ReadFile(path)
		if os.IsNotExist(err) {
			return nil
		}
		if err != nil {
			t.Fatal(err)
		}
		var ns []notifier.Notification
		for _, line := range strings.Split(strings.TrimSpace(string(body)), "\n") {
			var n notifier.Notification
			if err := json.Unmarshal([]byte(line), &n); err != nil {
				t.Fatal(err)
			}
			ns = append(ns, n)
		}
		return ns
	}
	reopen()
	for _, seconds := range []int{0, 299, 300, 360} {
		if seconds == 300 {
			reopen()
		}
		_, err := processSourceSweep(context.Background(), cfg, now.Add(time.Duration(seconds)*time.Second), monitor, engine)
		if err == nil {
			t.Fatal("missing source treated as healthy")
		}
		want := 0
		if seconds >= 300 {
			want = 1
		}
		ns := readSink()
		if len(ns) != want || store.IsProcessed("old-open") || store.BootstrapComplete() {
			t.Fatal("unhealthy source replayed producer events or duplicated warning")
		}
		if want == 1 && ns[0].EventType != "ALERT_SOURCE_STOPPED" {
			t.Fatal("sent old source alert")
		}
	}
	// Healthy source admits normal bootstrap after delivering recovery.
	healthyAt := now.Add(7 * time.Minute)
	body, _ := json.Marshal(alertstore.Heartbeat{Version: "step43-v1", LastSuccessAt: healthyAt.Format(time.RFC3339)})
	if err := os.WriteFile(filepath.Join(sourceDir, "status.json"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	stats, err := processSourceSweep(context.Background(), cfg, healthyAt, monitor, engine)
	if err != nil {
		t.Fatal(err)
	}
	ns := readSink()
	if len(ns) != 3 || ns[1].Transition != "RESOLVED" || ns[2].SourceEventID != "old-open" || !stats.Bootstrap {
		t.Fatalf("bootstrap/recovery changed: %+v %+v", ns, stats)
	}
}
