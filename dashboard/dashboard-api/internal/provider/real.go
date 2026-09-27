package provider

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	hldiag "control-dashboard-api/internal/integration/hyperliquid"
	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	sourceprobe "control-dashboard-api/internal/integration/probe"
	sqlitemarket "control-dashboard-api/internal/integration/sqlite"
)

// RealConfig configures read-only source access. No command/control endpoint is
// exposed by this provider.
type RealConfig struct {
	PostgreSQLDSN          string
	NATSURL                string
	NATSMonitorURL         string
	NATSStream             string
	MarketDataDB           string
	RuntimeMode            string
	MarketTopN             int
	MarketHistoryDays      int
	StrategyUniverseN      int
	ProbeTimeout           time.Duration
	ExecutionVenue         string
	VenueTargetEnvironment string
	ExchangeGatewayMode    string
	VenuePublicInfoURL     string
	VenuePublicTimeout     time.Duration
	// Deferred Step 37 compatibility fields.
	// Step 37A does not require or consume private credentials, but keeping the
	// public account/agent addresses in the config shape makes cumulative
	// dashboard upgrades robust if Step 37 read-only probe files are still
	// present from an earlier extraction. No private key is accepted here.
	VenueAccountAddress   string
	VenueAPIWalletAddress string
	AlertStoreDir         string
	ManualAuditDir        string
	AlertAckDir           string
}

type Real struct {
	cfg         RealConfig
	prober      *sourceprobe.Prober
	postgres    *pgstore.Reader
	natsMonitor *natsdiag.Monitor
	jetstream   *natsdiag.JetStreamReader
	subscriber  *natsdiag.SubjectSubscriber
	marketData  *sqlitemarket.Reader
	venuePublic *hldiag.PublicClient

	// Step 44.5: several dashboard read models depend on the same relatively
	// expensive canonical SQLite/PostgreSQL snapshots. A single readiness HTTP
	// request fans out through shell/alerts/registry/rules, so without
	// coalescing it can launch duplicate reads against the same sources and
	// create its own timeouts. These caches are short-lived observability
	// snapshots only; they never alter trading state or routing decisions.
	marketCache readModelMarketCache
	infraCache  readModelInfraCache
	alertsCache readModelAlertsCache
}

type readModelMarketCache struct {
	mu       sync.Mutex
	inFlight bool
	done     chan struct{}
	value    realMarketData
	err      error
	finished time.Time
}

type readModelInfraCache struct {
	mu       sync.Mutex
	inFlight bool
	done     chan struct{}
	value    realInfrastructure
	finished time.Time
}

type readModelAlertsCache struct {
	mu       sync.Mutex
	inFlight bool
	done     chan struct{}
	value    realAlertsAuditData
	finished time.Time
}

const dashboardReadCoalesceTTL = 2 * time.Second

func NewReal(cfg RealConfig) *Real {
	return &Real{
		cfg: cfg,
		prober: sourceprobe.New(sourceprobe.Config{
			PostgreSQLDSN: cfg.PostgreSQLDSN,
			NATSURL:       cfg.NATSURL,
			MarketDataDB:  cfg.MarketDataDB,
			Timeout:       cfg.ProbeTimeout,
		}),
		postgres: pgstore.NewReader(pgstore.Config{
			DSN:     cfg.PostgreSQLDSN,
			Timeout: maxDuration(cfg.ProbeTimeout, 2*time.Second),
		}),
		natsMonitor: natsdiag.NewMonitor(cfg.NATSMonitorURL, maxDuration(cfg.ProbeTimeout, 1500*time.Millisecond)),
		jetstream:   natsdiag.NewJetStreamReader(cfg.NATSURL, cfg.NATSStream, maxDuration(cfg.ProbeTimeout, 1500*time.Millisecond)),
		subscriber:  natsdiag.NewSubjectSubscriber(cfg.NATSURL, natsdiag.AuditedRuntimeSubjects, maxDuration(cfg.ProbeTimeout, 1500*time.Millisecond)),
		marketData: sqlitemarket.NewReader(sqlitemarket.ReaderConfig{
			DatabasePath: cfg.MarketDataDB,
			Timeout:      maxDuration(cfg.ProbeTimeout, 2*time.Second),
		}),
		venuePublic: hldiag.NewPublicClient(cfg.VenuePublicInfoURL, cfg.VenuePublicTimeout),
	}
}

func (p *Real) Read(ctx context.Context, resource Resource) (json.RawMessage, error) {
	switch resource {
	case ResourceShellStatus:
		value := p.shellStatus(ctx)
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real shell-status resource: %w", err)
		}
		return raw, nil
	case ResourceOverview:
		value, err := p.overview(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real overview resource: %w", err)
		}
		return raw, nil
	case ResourceInfrastructure:
		value := p.infrastructure(ctx)
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real infrastructure resource: %w", err)
		}
		return raw, nil
	case ResourcePositions:
		value, err := p.positions(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real positions resource: %w", err)
		}
		return raw, nil
	case ResourceReconciliation:
		value, err := p.reconciliation(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real reconciliation resource: %w", err)
		}
		return raw, nil
	case ResourceExecution:
		value, err := p.execution(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real execution resource: %w", err)
		}
		return raw, nil
	case ResourcePipeline:
		value, err := p.pipeline(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real pipeline resource: %w", err)
		}
		return raw, nil
	case ResourceRisk:
		value, err := p.risk(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real risk resource: %w", err)
		}
		return raw, nil
	case ResourceMarketData:
		value, err := p.marketDataResource(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real market-data resource: %w", err)
		}
		return raw, nil
	case ResourceAlertsAudit:
		value := p.alertsAudit(ctx)
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real alerts-audit resource: %w", err)
		}
		return raw, nil
	case ResourceLiveVsExpected:
		value, err := p.liveVsExpected(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode real live-vs-expected resource: %w", err)
		}
		return raw, nil
	case ResourceManualControl:
		value, err := p.manualControlBase(ctx)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(value)
		if err != nil {
			return nil, fmt.Errorf("encode manual-control resource: %w", err)
		}
		return raw, nil
	default:
		return nil, fmt.Errorf("%w: %s", ErrIntegrationNotImplemented, resource)
	}
}

func (p *Real) Health(_ context.Context) Health {
	configured := 0
	if p.cfg.PostgreSQLDSN != "" {
		configured++
	}
	if p.cfg.NATSURL != "" {
		configured++
	}
	if p.cfg.MarketDataDB != "" {
		configured++
	}

	return Health{
		Name:          "real-data-provider-step46",
		Mode:          "real",
		Ready:         false,
		Detail:        fmt.Sprintf("sources configured %d/3; Step 46 completes the dashboard-side manual routing admission/audit contract while private auth, trading-control sink and order routing remain deferred/disabled", configured),
		ResourceCount: len(AllResources),
	}
}

// SubscribeDashboardEvents bridges canonical trading-runtime subjects into
// dashboard invalidations. It is read-only: the subscriber never publishes or
// creates JetStream consumers.
func (p *Real) SubscribeDashboardEvents(ctx context.Context, onSubject func(string)) error {
	if p.subscriber == nil {
		return fmt.Errorf("dashboard NATS subscriber is not configured")
	}
	return p.subscriber.Run(ctx, onSubject)
}

// SourceStatus performs bounded, read-only reachability checks. It never
// mutates trading state and is intentionally separate from /api/health.
func (p *Real) SourceStatus(ctx context.Context) sourceprobe.Status {
	return p.prober.Check(ctx)
}

func (p *Real) RuntimeStateSummary(ctx context.Context) map[string]any {
	result := map[string]any{"mode": "real"}
	snapshot, err := p.postgres.Snapshot(ctx)
	if err != nil {
		result["available"] = false
		result["error"] = err.Error()
		return result
	}
	result["available"] = true
	result["postgres"] = snapshot
	return result
}

func (p *Real) runtimeMode() string {
	mode := strings.ToUpper(strings.TrimSpace(p.cfg.RuntimeMode))
	switch mode {
	case "LIVE", "TESTNET", "REPLAY":
		return mode
	default:
		return "LIVE"
	}
}

func maxDuration(value, minimum time.Duration) time.Duration {
	if value < minimum {
		return minimum
	}
	return value
}
