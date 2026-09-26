package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"control-dashboard-api/internal/alertstore"
	"control-dashboard-api/internal/provider"
)

const version = "0.43.0"

type alertsEnvelope struct {
	Alerts []alertstore.Alert `json:"alerts"`
}

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
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
	timeout, err := time.ParseDuration(envOr("DASHBOARD_WATCHDOG_SWEEP_TIMEOUT", "8s"))
	if err != nil || timeout <= 0 || timeout > 30*time.Second {
		logger.Error("invalid DASHBOARD_WATCHDOG_SWEEP_TIMEOUT", "value", os.Getenv("DASHBOARD_WATCHDOG_SWEEP_TIMEOUT"))
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	logger.Info("dashboard watchdog starting", "version", version, "interval", interval.String(), "store", storeDir)

	sweep := func() {
		now := time.Now().UTC()
		sweepCtx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		raw, err := real.Read(sweepCtx, provider.ResourceAlertsAudit)
		if err != nil {
			_ = store.RecordFailure(now, err)
			logger.Warn("watchdog sweep failed", "error", err)
			return
		}
		var envelope alertsEnvelope
		if err := json.Unmarshal(raw, &envelope); err != nil {
			_ = store.RecordFailure(now, err)
			logger.Warn("watchdog decode failed", "error", err)
			return
		}
		hb, err := store.Apply(now, envelope.Alerts)
		if err != nil {
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
	publicTimeout, err := time.ParseDuration(envOr("DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT", "3s"))
	if err != nil || publicTimeout <= 0 || publicTimeout > 10*time.Second {
		return nil, fmt.Errorf("invalid DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT")
	}

	venue := strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_EXECUTION_VENUE", "HYPERLIQUID")))
	environment := strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "TESTNET")))
	gateway := strings.ToLower(strings.TrimSpace(envOr("DASHBOARD_EXCHANGE_GATEWAY_MODE", "hyperliquid-dry-run")))
	publicURL := strings.TrimSpace(envOr("DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL", "https://api.hyperliquid-testnet.xyz/info"))
	if venue != "HYPERLIQUID" || environment != "TESTNET" || gateway != "hyperliquid-dry-run" {
		return nil, fmt.Errorf("watchdog must remain on HYPERLIQUID TESTNET hyperliquid-dry-run")
	}
	if err := validateTestnetInfoURL(publicURL); err != nil {
		return nil, err
	}

	return provider.NewReal(provider.RealConfig{
		PostgreSQLDSN:          strings.TrimSpace(os.Getenv("DASHBOARD_POSTGRES_DSN")),
		NATSURL:                strings.TrimSpace(os.Getenv("DASHBOARD_NATS_URL")),
		NATSMonitorURL:         strings.TrimSpace(os.Getenv("DASHBOARD_NATS_MONITOR_URL")),
		NATSStream:             strings.TrimSpace(envOr("DASHBOARD_NATS_STREAM", "ALGOTRADING_RUNTIME")),
		MarketDataDB:           strings.TrimSpace(os.Getenv("DASHBOARD_MARKET_DATA_DB")),
		RuntimeMode:            strings.TrimSpace(envOr("DASHBOARD_RUNTIME_MODE", "LIVE")),
		MarketTopN:             marketTopN,
		MarketHistoryDays:      historyDays,
		StrategyUniverseN:      universeN,
		ProbeTimeout:           probeTimeout,
		ExecutionVenue:         venue,
		VenueTargetEnvironment: environment,
		ExchangeGatewayMode:    gateway,
		VenuePublicInfoURL:     publicURL,
		VenuePublicTimeout:     publicTimeout,
		// Intentionally omit AlertStoreDir: the watchdog derives current alerts
		// from canonical sources and must not feed its own durable history back
		// into the sweep input.
	}), nil
}

func validateTestnetInfoURL(raw string) error {
	parsed, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return fmt.Errorf("invalid DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL: %w", err)
	}
	if parsed.Scheme != "https" || parsed.Hostname() != "api.hyperliquid-testnet.xyz" || parsed.Port() != "" || parsed.Path != "/info" || parsed.RawQuery != "" || parsed.Fragment != "" || parsed.User != nil {
		return fmt.Errorf("DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL must be exactly the HTTPS Hyperliquid testnet /info endpoint")
	}
	return nil
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
