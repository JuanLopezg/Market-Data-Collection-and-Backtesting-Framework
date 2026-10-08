package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"control-dashboard-api/internal/provider"
	"control-dashboard-api/internal/server"
)

const version = "0.46.1"

func main() {
	var healthcheck bool
	flag.BoolVar(&healthcheck, "healthcheck", false, "check the local dashboard-api liveness endpoint")
	flag.Parse()

	if healthcheck {
		if err := runHealthcheck(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	users, err := configuredUsers()
	if err != nil {
		logger.Error("invalid authentication configuration", "error", err)
		os.Exit(1)
	}

	sessionTTL, err := time.ParseDuration(envOr("DASHBOARD_SESSION_TTL", "8h"))
	if err != nil || sessionTTL <= 0 {
		logger.Error("invalid DASHBOARD_SESSION_TTL", "value", os.Getenv("DASHBOARD_SESSION_TTL"))
		os.Exit(1)
	}

	dataProvider, err := configuredProvider()
	if err != nil {
		logger.Error("invalid dashboard data provider configuration", "error", err)
		os.Exit(1)
	}

	resourceTimeout, err := time.ParseDuration(envOr("DASHBOARD_RESOURCE_TIMEOUT", "7s"))
	if err != nil || resourceTimeout <= 0 {
		logger.Error("invalid DASHBOARD_RESOURCE_TIMEOUT", "value", os.Getenv("DASHBOARD_RESOURCE_TIMEOUT"))
		os.Exit(1)
	}
	sseMaxClients, err := envPositiveInt("DASHBOARD_SSE_MAX_CLIENTS", 16)
	if err != nil {
		logger.Error("invalid DASHBOARD_SSE_MAX_CLIENTS", "error", err)
		os.Exit(1)
	}

	api, err := server.New(server.Config{
		Addr:            envOr("DASHBOARD_API_ADDR", ":8080"),
		Version:         version,
		Logger:          logger,
		Users:           users,
		SessionTTL:      sessionTTL,
		CookieSecure:    envBool("DASHBOARD_AUTH_COOKIE_SECURE", false),
		DemoAuthAllowed: envBool("DASHBOARD_AUTH_ALLOW_DEMO", false),
		Provider:        dataProvider,
		ResourceTimeout: resourceTimeout,
		SSEMaxClients:   sseMaxClients,
		ManualAuditDir:  strings.TrimSpace(os.Getenv("DASHBOARD_MANUAL_AUDIT_DIR")),
		AlertAckDir:     strings.TrimSpace(os.Getenv("DASHBOARD_ALERT_ACK_DIR")),
		AlertStoreDir:   strings.TrimSpace(envOr("DASHBOARD_ALERT_STORE_DIR", "/data/watchdog")),
	})
	if err != nil {
		logger.Error("failed to initialize dashboard api", "error", err)
		os.Exit(1)
	}

	providerHealth := dataProvider.Health(context.Background())
	httpServer := api.HTTPServer()
	errCh := make(chan error, 1)
	go func() {
		logger.Info(
			"dashboard api starting",
			"addr", httpServer.Addr,
			"version", version,
			"provider", providerHealth.Name,
			"provider_mode", providerHealth.Mode,
			"provider_ready", providerHealth.Ready,
			"auth", "session-cookie",
		)
		errCh <- httpServer.ListenAndServe()
	}()

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGINT, syscall.SIGTERM)

	select {
	case sig := <-signals:
		logger.Info("shutdown requested", "signal", sig.String())
	case err := <-errCh:
		if !errors.Is(err, http.ErrServerClosed) {
			logger.Error("dashboard api stopped unexpectedly", "error", err)
			os.Exit(1)
		}
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := httpServer.Shutdown(ctx); err != nil {
		logger.Error("dashboard api shutdown failed", "error", err)
		os.Exit(1)
	}
	logger.Info("dashboard api stopped")
}

func configuredProvider() (provider.Provider, error) {
	mode := strings.ToLower(strings.TrimSpace(envOr("DASHBOARD_DATA_PROVIDER", "mock")))
	switch mode {
	case "mock":
		return provider.NewEmbeddedMock()
	case "simulation":
		dir := strings.TrimSpace(envOr("DASHBOARD_SIMULATION_DIR", "/data/simulation"))
		if dir == "" {
			return nil, fmt.Errorf("DASHBOARD_SIMULATION_DIR must not be empty")
		}
		return provider.NewSimulation(provider.SimulationConfig{Dir: dir}), nil
	case "real":
		// Step 36 keeps all prior read-only/safety boundaries and adds public Hyperliquid TESTNET trading-rule validation for supported mappings only.
		// The proof never creates trades; it only correlates already-durable Strategy/Risk/Planner/Execution/Fill/Reconciliation evidence.
		probeTimeout, err := time.ParseDuration(envOr("DASHBOARD_SOURCE_PROBE_TIMEOUT", "1s"))
		if err != nil || probeTimeout <= 0 {
			return nil, fmt.Errorf("invalid DASHBOARD_SOURCE_PROBE_TIMEOUT")
		}
		marketTopN, err := envPositiveInt("DASHBOARD_MARKET_TOP_N", 50)
		if err != nil {
			return nil, err
		}
		marketHistoryDays, err := envPositiveInt("DASHBOARD_MARKET_HISTORY_DAYS", 100)
		if err != nil {
			return nil, err
		}
		strategyUniverseN, err := envPositiveInt("DASHBOARD_STRATEGY_UNIVERSE_N", 20)
		if err != nil {
			return nil, err
		}
		executionVenue := strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_EXECUTION_VENUE", "HYPERLIQUID")))
		venueEnvironment := strings.ToUpper(strings.TrimSpace(envOr("DASHBOARD_VENUE_TARGET_ENVIRONMENT", "TESTNET")))
		gatewayMode := strings.ToLower(strings.TrimSpace(envOr("DASHBOARD_EXCHANGE_GATEWAY_MODE", "hyperliquid-dry-run")))
		venuePublicInfoURL := strings.TrimSpace(envOr("DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL", "https://api.hyperliquid-testnet.xyz/info"))
		venuePublicTimeout, err := time.ParseDuration(envOr("DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT", "3s"))
		if err != nil || venuePublicTimeout <= 0 || venuePublicTimeout > 10*time.Second {
			return nil, fmt.Errorf("invalid DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT")
		}
		paper := strings.TrimSpace(os.Getenv("DASHBOARD_RUNTIME_MODE")) == "PAPER" &&
			executionVenue == "SIMULATED" && venueEnvironment == "PAPER" && gatewayMode == "backend"
		if strings.TrimSpace(os.Getenv("DASHBOARD_RUNTIME_MODE")) == "PAPER" && !paper {
			return nil, fmt.Errorf("PAPER requires SIMULATED venue, PAPER environment and backend gateway")
		}
		if !paper && executionVenue != "HYPERLIQUID" {
			return nil, fmt.Errorf("Step 36 current audited execution venue must be HYPERLIQUID, got %q", executionVenue)
		}
		if !paper && venueEnvironment != "TESTNET" {
			return nil, fmt.Errorf("Step 36 target environment must be TESTNET, got %q", venueEnvironment)
		}
		if !paper && gatewayMode != "hyperliquid-dry-run" {
			return nil, fmt.Errorf("Step 36 gateway mode must remain hyperliquid-dry-run, got %q", gatewayMode)
		}
		if paper {
			venuePublicInfoURL = ""
		} else if err := validateHyperliquidTestnetInfoURL(venuePublicInfoURL); err != nil {
			return nil, err
		}

		return provider.NewReal(provider.RealConfig{
			HostMetricsFile:        strings.TrimSpace(os.Getenv("DASHBOARD_HOST_METRICS_FILE")),
			HostMetricsProject:     strings.TrimSpace(os.Getenv("DASHBOARD_HOST_METRICS_PROJECT")),
			PostgreSQLDSN:          strings.TrimSpace(os.Getenv("DASHBOARD_POSTGRES_DSN")),
			NATSURL:                strings.TrimSpace(os.Getenv("DASHBOARD_NATS_URL")),
			NATSMonitorURL:         strings.TrimSpace(os.Getenv("DASHBOARD_NATS_MONITOR_URL")),
			NATSStream:             strings.TrimSpace(envOr("DASHBOARD_NATS_STREAM", "ALGOTRADING_RUNTIME")),
			MarketDataDB:           strings.TrimSpace(os.Getenv("DASHBOARD_MARKET_DATA_DB")),
			RuntimeMode:            strings.TrimSpace(envOr("DASHBOARD_RUNTIME_MODE", "LIVE")),
			MarketTopN:             marketTopN,
			MarketHistoryDays:      marketHistoryDays,
			QuoteVolume:            os.Getenv("DASHBOARD_QUOTE_VOLUME") == "true",
			QuoteVolumeFrom:        strings.TrimSpace(os.Getenv("DASHBOARD_QUOTE_VOLUME_FROM")),
			StrategyUniverseN:      strategyUniverseN,
			ProbeTimeout:           probeTimeout,
			ExecutionVenue:         executionVenue,
			VenueTargetEnvironment: venueEnvironment,
			ExchangeGatewayMode:    gatewayMode,
			VenuePublicInfoURL:     venuePublicInfoURL,
			VenuePublicTimeout:     venuePublicTimeout,
			AlertStoreDir:          strings.TrimSpace(envOr("DASHBOARD_ALERT_STORE_DIR", "/data/watchdog")),
			ManualAuditDir:         strings.TrimSpace(os.Getenv("DASHBOARD_MANUAL_AUDIT_DIR")),
			AlertAckDir:            strings.TrimSpace(os.Getenv("DASHBOARD_ALERT_ACK_DIR")),
		}), nil
	default:
		return nil, fmt.Errorf("unsupported DASHBOARD_DATA_PROVIDER %q; expected mock, simulation or real", mode)
	}
}

func validateHyperliquidTestnetInfoURL(raw string) error {
	parsed, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return fmt.Errorf("invalid DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL: %w", err)
	}
	if parsed.Scheme != "https" || parsed.Hostname() != "api.hyperliquid-testnet.xyz" || parsed.Port() != "" || parsed.Path != "/info" || parsed.RawPath != "" || parsed.RawQuery != "" || parsed.ForceQuery || parsed.Fragment != "" || parsed.User != nil {
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

func configuredUsers() ([]server.AuthUser, error) {
	viewerUsername := strings.TrimSpace(envOr("DASHBOARD_VIEWER_USERNAME", "viewer"))
	operatorUsername := strings.TrimSpace(envOr("DASHBOARD_OPERATOR_USERNAME", "operator"))
	viewerPassword := os.Getenv("DASHBOARD_VIEWER_PASSWORD")
	operatorPassword := os.Getenv("DASHBOARD_OPERATOR_PASSWORD")

	if envBool("DASHBOARD_AUTH_ALLOW_DEMO", false) {
		if viewerPassword == "" {
			viewerPassword = "viewer-demo"
		}
		if operatorPassword == "" {
			operatorPassword = "operator-demo"
		}
	}

	if viewerPassword == "" && operatorPassword == "" {
		return nil, fmt.Errorf("set DASHBOARD_VIEWER_PASSWORD and/or DASHBOARD_OPERATOR_PASSWORD; demo credentials are disabled")
	}

	users := make([]server.AuthUser, 0, 2)
	if viewerPassword != "" {
		users = append(users, server.AuthUser{Username: viewerUsername, Password: viewerPassword, Role: server.RoleViewer})
	}
	if operatorPassword != "" {
		users = append(users, server.AuthUser{Username: operatorUsername, Password: operatorPassword, Role: server.RoleOperator})
	}
	return users, nil
}

func runHealthcheck() error {
	client := &http.Client{Timeout: 2 * time.Second}
	response, err := client.Get(envOr("DASHBOARD_API_HEALTH_URL", "http://127.0.0.1:8080/api/health"))
	if err != nil {
		return fmt.Errorf("healthcheck request failed: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("healthcheck returned HTTP %d", response.StatusCode)
	}
	return nil
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func envBool(name string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return fallback
	}
	return parsed
}
