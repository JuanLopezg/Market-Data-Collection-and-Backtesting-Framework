package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func clearNotifierEnv(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		"DASHBOARD_ALERT_STORE_DIR", "DASHBOARD_NOTIFIER_STORE_DIR", "DASHBOARD_NOTIFIER_TEST_SINK_FILE",
		"DASHBOARD_NOTIFIER_SINK", "DASHBOARD_NOTIFIER_MIN_SEVERITY", "DASHBOARD_NOTIFIER_INTERVAL",
		"DASHBOARD_NOTIFIER_COOLDOWN", "DASHBOARD_NOTIFIER_SWEEP_TIMEOUT", "DASHBOARD_NOTIFIER_SOURCE_MAX_BYTES",
		"DASHBOARD_TELEGRAM_BOT_TOKEN", "DASHBOARD_TELEGRAM_BOT_TOKEN_FILE", "DASHBOARD_TELEGRAM_CHAT_ID",
		"DASHBOARD_TELEGRAM_CHAT_ID_FILE", "DASHBOARD_TELEGRAM_RECEIPT_FILE", "DASHBOARD_TELEGRAM_REQUEST_TIMEOUT",
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
	cfg, err := loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.sink != "TELEGRAM" || cfg.telegramBotToken == "" || cfg.telegramChatID == "" || cfg.telegramRequestTimeout != 7*time.Second {
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
