package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"control-dashboard-api/internal/alertstore"
	krakenpublic "control-dashboard-api/internal/integration/kraken"
	"control-dashboard-api/internal/provider"
)

const version = "0.43.0"

type alertsEnvelope struct {
	Alerts []alertstore.Alert `json:"alerts"`
}

func main() {
	logLevel := slog.LevelInfo
	if debug, _ := strconv.ParseBool(os.Getenv("ALGOTRADING_LOG_DEBUG")); debug {
		logLevel = slog.LevelDebug
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: logLevel}))
	real, err := configuredRealProvider()
	if err != nil {
		logger.Error("invalid watchdog provider configuration", "error", err)
		os.Exit(1)
	}
	storeDir := strings.TrimSpace(envOr("DASHBOARD_ALERT_STORE_DIR", "/data/watchdog"))
	store, err := alertstore.Open(storeDir)
	if err != nil {
		logger.Error("open durable alert store", "error", err)
		os.Exit(1)
	}
	interval, err := time.ParseDuration(envOr("DASHBOARD_WATCHDOG_INTERVAL", "15s"))
	if err != nil || interval < 5*time.Second || interval > 5*time.Minute {
		logger.Error("invalid DASHBOARD_WATCHDOG_INTERVAL", "value", os.Getenv("DASHBOARD_WATCHDOG_INTERVAL"))
		os.Exit(1)
	}
	timeout, err := time.ParseDuration(envOr("DASHBOARD_WATCHDOG_SWEEP_TIMEOUT", "12s"))
	if err != nil || timeout <= 0 || timeout > 30*time.Second {
		logger.Error("invalid DASHBOARD_WATCHDOG_SWEEP_TIMEOUT", "value", os.Getenv("DASHBOARD_WATCHDOG_SWEEP_TIMEOUT"))
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	logger.Info("dashboard watchdog starting", "version", version, "interval", interval.String(), "store", storeDir)
	debouncer := newSourceDebouncer(alertstore.ReadSnapshot(storeDir, 1).Active)

	sweep := func() {
		now := time.Now().UTC()
		sweepCtx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		raw, err := real.Read(sweepCtx, provider.ResourceAlertsAudit)
		if err != nil {
			debouncer.interrupt()
			_ = store.RecordFailure(now, err)
			logger.Warn("watchdog sweep failed", "error", err)
			return
		}
		var envelope alertsEnvelope
		if err := json.Unmarshal(raw, &envelope); err != nil {
			debouncer.interrupt()
			_ = store.RecordFailure(now, err)
			logger.Warn("watchdog decode failed", "error", err)
			return
		}
		previousStreaks := debouncer.streaks
		hb, err := store.Apply(now, debouncer.filter(envelope.Alerts))
		if err != nil {
			debouncer.streaks = previousStreaks
			debouncer.interrupt()
			_ = store.RecordFailure(now, err)
			logger.Error("watchdog persistence failed", "error", err)
			return
		}
		logger.Info("watchdog sweep", "active", hb.ActiveCount, "critical", hb.ActiveCritical, "warnings", hb.ActiveWarnings, "durable_events", hb.EventCount)
	}

	sweep()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			logger.Info("dashboard watchdog stopped")
			return
		case <-ticker.C:
			sweep()
		}
	}
}

func configuredRealProvider() (*provider.Real, error) {
	queryTimeout, err := time.ParseDuration(envOr("DASHBOARD_POSTGRES_QUERY_TIMEOUT", "8s"))
	if err != nil || queryTimeout <= 0 {
		return nil, fmt.Errorf("invalid DASHBOARD_POSTGRES_QUERY_TIMEOUT")
	}
	connectTimeout, err := time.ParseDuration(envOr("DASHBOARD_POSTGRES_CONNECT_TIMEOUT", "5s"))
	if err != nil || connectTimeout <= 0 {
		return nil, fmt.Errorf("invalid DASHBOARD_POSTGRES_CONNECT_TIMEOUT")
	}
	probeTimeout, err := time.ParseDuration(envOr("DASHBOARD_SOURCE_PROBE_TIMEOUT", "1s"))
	if err != nil || probeTimeout <= 0 {
		return nil, fmt.Errorf("invalid DASHBOARD_SOURCE_PROBE_TIMEOUT")
	}
	marketTopN, err := envPositiveInt("DASHBOARD_MARKET_TOP_N", 50)
	if err != nil {
		return nil, err
	}
	historyDays, err := envPositiveInt("DASHBOARD_MARKET_HISTORY_DAYS", 100)
	if err != nil {
		return nil, err
	}
	universeN, err := envPositiveInt("DASHBOARD_STRATEGY_UNIVERSE_N", 20)
	if err != nil {
		return nil, err
	}
	// Same chain boundary as dashboard-api: PAPER or Kraken LIVE only.
	runtimeMode := strings.TrimSpace(os.Getenv("DASHBOARD_RUNTIME_MODE"))
	venue := strings.ToUpper(strings.TrimSpace(os.Getenv("DASHBOARD_EXECUTION_VENUE")))
	environment := strings.ToUpper(strings.TrimSpace(os.Getenv("DASHBOARD_VENUE_TARGET_ENVIRONMENT")))
	gateway := strings.ToLower(strings.TrimSpace(os.Getenv("DASHBOARD_EXCHANGE_GATEWAY_MODE")))
	if err := provider.ValidateChainBoundary(runtimeMode, venue, environment, gateway); err != nil {
		return nil, err
	}

	return provider.NewReal(provider.RealConfig{
		HostMetricsFile:          strings.TrimSpace(os.Getenv("DASHBOARD_HOST_METRICS_FILE")),
		HostMetricsProject:       strings.TrimSpace(os.Getenv("DASHBOARD_HOST_METRICS_PROJECT")),
		PostgreSQLDSN:            strings.TrimSpace(os.Getenv("DASHBOARD_POSTGRES_DSN")),
		NATSURL:                  strings.TrimSpace(os.Getenv("DASHBOARD_NATS_URL")),
		NATSMonitorURL:           strings.TrimSpace(os.Getenv("DASHBOARD_NATS_MONITOR_URL")),
		NATSStream:               strings.TrimSpace(envOr("DASHBOARD_NATS_STREAM", "ALGOTRADING_RUNTIME")),
		MarketDataDB:             strings.TrimSpace(os.Getenv("DASHBOARD_MARKET_DATA_DB")),
		RuntimeMode:              strings.TrimSpace(envOr("DASHBOARD_RUNTIME_MODE", "LIVE")),
		MarketTopN:               marketTopN,
		MarketHistoryDays:        historyDays,
		QuoteVolume:              os.Getenv("DASHBOARD_QUOTE_VOLUME") == "true",
		QuoteVolumeFrom:          strings.TrimSpace(os.Getenv("DASHBOARD_QUOTE_VOLUME_FROM")),
		PaperComparisonFile:      strings.TrimSpace(os.Getenv("DASHBOARD_PAPER_COMPARISON_FILE")),
		RSIEntry:                 envFloat("DASHBOARD_RSI_ENTRY", 80),
		RSIExit:                  envFloat("DASHBOARD_RSI_EXIT", 70),
		StrategyUniverseN:        universeN,
		StrategyConfigFile:       strings.TrimSpace(os.Getenv("DASHBOARD_STRATEGY_CONFIG")),
		ProbeTimeout:             probeTimeout,
		PostgreSQLQueryTimeout:   queryTimeout,
		PostgreSQLConnectTimeout: connectTimeout,
		ExecutionVenue:           venue,
		SymbolMapFile:            strings.TrimSpace(os.Getenv("DASHBOARD_SYMBOL_MAP_FILE")),
		KrakenTickersURL:         krakenpublic.TickersURL,
		// Intentionally omit AlertStoreDir: the watchdog derives current alerts
		// from canonical sources and must not feed its own durable history back
		// into the sweep input.
	}), nil
}

func envPositiveInt(name string, fallback int) (int, error) {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed <= 0 {
		return 0, fmt.Errorf("invalid %s", name)
	}
	return parsed, nil
}
func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func envFloat(name string, fallback float64) float64 {
	value, err := strconv.ParseFloat(os.Getenv(name), 64)
	if err != nil {
		return fallback
	}
	return value
}
