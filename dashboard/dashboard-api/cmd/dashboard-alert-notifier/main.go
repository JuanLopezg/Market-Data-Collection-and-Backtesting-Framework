package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"control-dashboard-api/internal/alertstore"
	"control-dashboard-api/internal/notifier"
)

const version = "0.46.3"
const maxSecretFileBytes = 8 * 1024

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	cfg, err := loadConfig()
	if err != nil {
		logger.Error("invalid alert notifier configuration", "error", err)
		os.Exit(1)
	}
	store, err := notifier.OpenStore(cfg.storeDir)
	if err != nil {
		logger.Error("open durable notifier store", "error", err)
		os.Exit(1)
	}

	var sink notifier.Sink
	switch cfg.sink {
	case "TEST_FILE":
		sink, err = notifier.NewTestFileSink(cfg.testSinkFile)
	case "TELEGRAM":
		sink, err = notifier.NewTelegramSink(cfg.telegramBotToken, cfg.telegramChatID, cfg.telegramReceiptFile, cfg.telegramRequestTimeout)
	default:
		err = fmt.Errorf("unsupported notifier sink %q", cfg.sink)
	}
	if err != nil {
		logger.Error("configure notifier sink", "sink", cfg.sink, "error", err)
		os.Exit(1)
	}

	engine, err := notifier.NewEngine(store, sink, cfg.minSeverity, cfg.cooldown)
	if err != nil {
		logger.Error("configure notifier engine", "error", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	logger.Info("dashboard alert notifier starting",
		"version", version,
		"source", cfg.alertStoreDir,
		"store", cfg.storeDir,
		"sink", cfg.sink,
		"minSeverity", cfg.minSeverity,
		"cooldown", cfg.cooldown.String(),
		"interval", cfg.interval.String(),
	)

	sweep := func() {
		now := time.Now().UTC()
		if _, err := alertstore.ReadHeartbeat(cfg.alertStoreDir); err != nil {
			_ = store.RecordFailure(now, 0, sink.Name(), cfg.minSeverity, cfg.cooldown, err)
			logger.Warn("notifier source heartbeat unavailable", "error", err)
			return
		}
		events, err := alertstore.ReadLifecycleEvents(cfg.alertStoreDir, cfg.sourceMaxBytes)
		if err != nil {
			_ = store.RecordFailure(now, 0, sink.Name(), cfg.minSeverity, cfg.cooldown, err)
			logger.Warn("notifier source replay failed", "error", err)
			return
		}
		sweepCtx, cancel := context.WithTimeout(ctx, cfg.sweepTimeout)
		defer cancel()
		stats, err := engine.Process(sweepCtx, events)
		if err != nil {
			_ = store.RecordFailure(now, len(events), sink.Name(), cfg.minSeverity, cfg.cooldown, err)
			logger.Error("notifier sweep failed", "error", err, "sourceEvents", len(events), "sink", sink.Name())
			return
		}
		if err := store.RecordSuccess(now, len(events), sink.Name(), cfg.minSeverity, cfg.cooldown); err != nil {
			logger.Error("notifier heartbeat persistence failed", "error", err)
			return
		}
		logger.Info("notifier sweep complete",
			"sourceEvents", stats.SourceEvents,
			"processed", stats.Processed,
			"delivered", stats.Delivered,
			"suppressed", stats.Suppressed,
			"bootstrap", stats.Bootstrap,
			"sink", sink.Name(),
		)
	}

	sweep()
	ticker := time.NewTicker(cfg.interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			logger.Info("dashboard alert notifier stopped")
			return
		case <-ticker.C:
			sweep()
		}
	}
}

type config struct {
	alertStoreDir          string
	storeDir               string
	testSinkFile           string
	sink                   string
	minSeverity            string
	interval               time.Duration
	cooldown               time.Duration
	sweepTimeout           time.Duration
	sourceMaxBytes         int64
	telegramBotToken       string
	telegramChatID         string
	telegramReceiptFile    string
	telegramRequestTimeout time.Duration
}

func loadConfig() (config, error) {
	sinkName := strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_NOTIFIER_SINK", "TEST_FILE")))
	storeDir := strings.TrimSpace(os.Getenv("DASHBOARD_NOTIFIER_STORE_DIR"))
	if storeDir == "" {
		if sinkName == "TELEGRAM" {
			storeDir = "/data/notifier/telegram"
		} else {
			storeDir = "/data/notifier"
		}
	}
	cfg := config{
		alertStoreDir: strings.TrimSpace(envOr("DASHBOARD_ALERT_STORE_DIR", "/data/watchdog")),
		storeDir:      storeDir,
		testSinkFile:  strings.TrimSpace(envOr("DASHBOARD_NOTIFIER_TEST_SINK_FILE", "/data/notifier/test-sink.jsonl")),
		sink:          sinkName,
		minSeverity:   strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_NOTIFIER_MIN_SEVERITY", "WARN"))),
	}
	if cfg.alertStoreDir == "" || cfg.storeDir == "" {
		return config{}, fmt.Errorf("notifier source/store paths must be configured")
	}
	if cfg.sink != "TEST_FILE" && cfg.sink != "TELEGRAM" {
		return config{}, fmt.Errorf("DASHBOARD_NOTIFIER_SINK must be TEST_FILE or TELEGRAM")
	}
	if cfg.sink == "TEST_FILE" && cfg.testSinkFile == "" {
		return config{}, fmt.Errorf("TEST_FILE notifier sink path must be configured")
	}
	if cfg.minSeverity != "INFO" && cfg.minSeverity != "WARN" && cfg.minSeverity != "CRITICAL" {
		return config{}, fmt.Errorf("invalid DASHBOARD_NOTIFIER_MIN_SEVERITY %q", cfg.minSeverity)
	}
	var err error
	cfg.interval, err = durationEnv("DASHBOARD_NOTIFIER_INTERVAL", "5s", time.Second, 5*time.Minute)
	if err != nil {
		return config{}, err
	}
	cfg.cooldown, err = durationEnv("DASHBOARD_NOTIFIER_COOLDOWN", "5m", 0, 24*time.Hour)
	if err != nil {
		return config{}, err
	}
	cfg.sweepTimeout, err = durationEnv("DASHBOARD_NOTIFIER_SWEEP_TIMEOUT", "10s", time.Second, time.Minute)
	if err != nil {
		return config{}, err
	}
	cfg.sourceMaxBytes, err = int64Env("DASHBOARD_NOTIFIER_SOURCE_MAX_BYTES", 64<<20, 1<<20, 512<<20)
	if err != nil {
		return config{}, err
	}

	if cfg.sink == "TELEGRAM" {
		cfg.telegramBotToken, err = secretEnvOrFile("DASHBOARD_TELEGRAM_BOT_TOKEN", "DASHBOARD_TELEGRAM_BOT_TOKEN_FILE")
		if err != nil {
			return config{}, err
		}
		cfg.telegramChatID, err = secretEnvOrFile("DASHBOARD_TELEGRAM_CHAT_ID", "DASHBOARD_TELEGRAM_CHAT_ID_FILE")
		if err != nil {
			return config{}, err
		}
		if cfg.telegramBotToken == "" || cfg.telegramChatID == "" {
			return config{}, errors.New("TELEGRAM sink requires bot token and chat id via deployment env/secret files")
		}
		cfg.telegramReceiptFile = strings.TrimSpace(os.Getenv("DASHBOARD_TELEGRAM_RECEIPT_FILE"))
		if cfg.telegramReceiptFile == "" {
			cfg.telegramReceiptFile = strings.TrimRight(cfg.storeDir, "/") + "/telegram-receipts.jsonl"
		}
		cfg.telegramRequestTimeout, err = durationEnv("DASHBOARD_TELEGRAM_REQUEST_TIMEOUT", "8s", time.Second, time.Minute)
		if err != nil {
			return config{}, err
		}
	}
	return cfg, nil
}

func secretEnvOrFile(valueEnv, fileEnv string) (string, error) {
	direct := strings.TrimSpace(os.Getenv(valueEnv))
	filePath := strings.TrimSpace(os.Getenv(fileEnv))
	if direct != "" && filePath != "" {
		return "", fmt.Errorf("configure only one of %s or %s", valueEnv, fileEnv)
	}
	if filePath == "" {
		return direct, nil
	}
	f, err := os.Open(filePath)
	if err != nil {
		return "", fmt.Errorf("read secret file configured by %s: %w", fileEnv, err)
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return "", fmt.Errorf("stat secret file configured by %s: %w", fileEnv, err)
	}
	if stat.Size() <= 0 || stat.Size() > maxSecretFileBytes {
		return "", fmt.Errorf("secret file configured by %s has invalid size", fileEnv)
	}
	body, err := io.ReadAll(io.LimitReader(f, maxSecretFileBytes+1))
	if err != nil {
		return "", fmt.Errorf("read secret file configured by %s: %w", fileEnv, err)
	}
	if len(body) == 0 || len(body) > maxSecretFileBytes {
		return "", fmt.Errorf("secret file configured by %s has invalid size", fileEnv)
	}
	value := strings.TrimSpace(string(body))
	if value == "" {
		return "", fmt.Errorf("secret file configured by %s is empty", fileEnv)
	}
	return value, nil
}

func durationEnv(name, fallback string, min, max time.Duration) (time.Duration, error) {
	raw := strings.TrimSpace(envOr(name, fallback))
	value, err := time.ParseDuration(raw)
	if err != nil || value < min || value > max {
		return 0, fmt.Errorf("invalid %s", name)
	}
	return value, nil
}

func int64Env(name string, fallback, min, max int64) (int64, error) {
	raw := strings.TrimSpace(os.Getenv(name))
	if raw == "" {
		return fallback, nil
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < min || value > max {
		return 0, fmt.Errorf("invalid %s", name)
	}
	return value, nil
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
