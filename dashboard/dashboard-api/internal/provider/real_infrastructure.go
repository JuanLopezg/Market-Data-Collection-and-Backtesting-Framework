package provider

import (
	"context"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	sourceprobe "control-dashboard-api/internal/integration/probe"
)

type realInfrastructure struct {
	TelemetryObservedAt string                    `json:"telemetryObservedAt,omitempty"`
	Readiness           string                    `json:"readiness"`
	ReadinessReason     string                    `json:"readinessReason"`
	LastUpdated         string                    `json:"lastUpdated"`
	VPS                 realVPS                   `json:"vps"`
	Containers          []realContainer           `json:"containers"`
	Services            []realService             `json:"services"`
	Postgres            realPostgres              `json:"postgres"`
	NATS                realNATS                  `json:"nats"`
	Exchange            realExchange              `json:"exchange"`
	Outbox              realOutbox                `json:"outbox"`
	Dependencies        []realReadinessDependency `json:"dependencies"`
	SourceMode          string                    `json:"sourceMode,omitempty"`
	SourceNote          string                    `json:"sourceNote,omitempty"`
}

type realVPS struct {
	RAMLabel         string  `json:"ramLabel,omitempty"`
	DiskLabel        string  `json:"diskLabel,omitempty"`
	DiskAvailableGiB float64 `json:"diskAvailableGiB,omitempty"`
	ClockObserved    bool    `json:"clockObserved"`
	CPUPct           float64 `json:"cpuPct"`
	RAMPct           float64 `json:"ramPct"`
	DiskPct          float64 `json:"diskPct"`
	Load1m           float64 `json:"load1m"`
	NetworkRXLabel   string  `json:"networkRxLabel"`
	NetworkTXLabel   string  `json:"networkTxLabel"`
	UptimeLabel      string  `json:"uptimeLabel"`
	ClockOffsetLabel string  `json:"clockOffsetLabel"`
	ClockSynced      bool    `json:"clockSynced"`
	State            string  `json:"state"`
}

type realContainer struct {
	Name          string  `json:"name"`
	State         string  `json:"state"`
	Health        string  `json:"health"`
	RestartCount  int     `json:"restartCount"`
	UptimeLabel   string  `json:"uptimeLabel"`
	CPUPct        float64 `json:"cpuPct"`
	RAMMB         float64 `json:"ramMb"`
	LastHeartbeat string  `json:"lastHeartbeat"`
}

type realService struct {
	ProcessRunning *bool  `json:"processRunning,omitempty"`
	Service        string `json:"service"`
	Ready          bool   `json:"ready"`
	Mode           string `json:"mode"`
	LastEvent      string `json:"lastEvent"`
	LagLabel       string `json:"lagLabel"`
	Health         string `json:"health"`
}

type realPostgres struct {
	Connected         bool    `json:"connected"`
	LatencyMS         float64 `json:"latencyMs"`
	ActiveConnections int     `json:"activeConnections"`
	MaxConnections    int     `json:"maxConnections"`
	StorageUsedLabel  string  `json:"storageUsedLabel"`
	StoragePct        float64 `json:"storagePct"`
	PersistenceState  string  `json:"persistenceState"`
	State             string  `json:"state"`
}

type realNATS struct {
	Connected       bool   `json:"connected"`
	Streams         int    `json:"streams"`
	Consumers       int    `json:"consumers"`
	PendingMessages int    `json:"pendingMessages"`
	MaxConsumerLag  int    `json:"maxConsumerLag"`
	Redeliveries    int    `json:"redeliveries"`
	AckHealthLabel  string `json:"ackHealthLabel"`
	State           string `json:"state"`
}

type realExchange struct {
	Venue           string  `json:"venue"`
	Connected       bool    `json:"connected"`
	ReconnectState  string  `json:"reconnectState"`
	LastAPIActivity string  `json:"lastApiActivity"`
	LastWSActivity  string  `json:"lastWsActivity"`
	APILatencyMS    float64 `json:"apiLatencyMs"`
	State           string  `json:"state"`
}

type realOutbox struct {
	PendingMessages int    `json:"pendingMessages"`
	OldestAgeLabel  string `json:"oldestAgeLabel"`
	LastPublished   string `json:"lastPublished"`
	State           string `json:"state"`
}

type realReadinessDependency struct {
	Component string `json:"component"`
	State     string `json:"state"`
	Reason    string `json:"reason"`
	LastCheck string `json:"lastCheck"`
}

func (p *Real) infrastructure(ctx context.Context) realInfrastructure {
	// Infrastructure is requested independently by shell, alerts and global
	// readiness. Coalesce those near-simultaneous reads so the dashboard does not
	// create duplicate TCP/PostgreSQL/NATS probes against its own dependencies.
	p.infraCache.mu.Lock()
	if !p.infraCache.finished.IsZero() && time.Since(p.infraCache.finished) < dashboardReadCoalesceTTL {
		value := p.infraCache.value
		p.infraCache.mu.Unlock()
		return value
	}
	if p.infraCache.inFlight {
		done := p.infraCache.done
		p.infraCache.mu.Unlock()
		select {
		case <-done:
			p.infraCache.mu.Lock()
			value := p.infraCache.value
			p.infraCache.mu.Unlock()
			return value
		case <-ctx.Done():
			return realInfrastructure{Readiness: "DEGRADED", ReadinessReason: "Infrastructure snapshot wait cancelled: " + ctx.Err().Error(), LastUpdated: time.Now().UTC().Format("15:04:05 UTC")}
		}
	}
	p.infraCache.inFlight = true
	p.infraCache.done = make(chan struct{})
	done := p.infraCache.done
	p.infraCache.mu.Unlock()

	value := p.loadInfrastructure(ctx)

	p.infraCache.mu.Lock()
	p.infraCache.value = value
	p.infraCache.finished = time.Now()
	p.infraCache.inFlight = false
	close(done)
	p.infraCache.mu.Unlock()
	return value
}

func (p *Real) loadInfrastructure(ctx context.Context) realInfrastructure {
	checkedAt := time.Now().UTC()
	sources := p.SourceStatus(ctx)

	var pg pgstore.OperationalSnapshot
	var pgErr error
	if sources.PostgreSQL.Reachable {
		pg, pgErr = p.postgres.Snapshot(ctx)
	}

	var natsStatus natsdiag.MonitorStatus
	var natsErr error
	if sources.NATS.Reachable && p.cfg.NATSMonitorURL != "" {
		natsStatus, natsErr = p.natsMonitor.Check(ctx)
	}

	result := realInfrastructure{
		Readiness:       "DEGRADED",
		ReadinessReason: "Partial real observability: source health is real; full trading readiness is not asserted yet.",
		LastUpdated:     checkedAt.Format("15:04:05 UTC"),
		SourceMode:      "REAL",
		SourceNote:      "PostgreSQL/NATS/market-data are queried read-only. Optional host telemetry is collected separately. Private exchange connectivity and live routing readiness are not asserted.",
		VPS: realVPS{
			NetworkRXLabel:   "Not wired",
			NetworkTXLabel:   "Not wired",
			UptimeLabel:      "Not wired",
			ClockOffsetLabel: "Not wired",
			ClockSynced:      false,
			State:            "UNKNOWN",
		},
		Containers: []realContainer{},
		Services:   []realService{},
		Exchange: realExchange{
			Venue:           strings.ToUpper(strings.TrimSpace(p.cfg.ExecutionVenue)),
			Connected:       false,
			ReconnectState:  "Foundation only · public connectivity not checked until Step 34",
			LastAPIActivity: "—",
			LastWSActivity:  "—",
			State:           "UNKNOWN",
		},
		Outbox: realOutbox{
			PendingMessages: -1,
			OldestAgeLabel:  "Not wired",
			LastPublished:   "—",
			State:           "UNKNOWN",
		},
	}

	result.Postgres = buildPostgresHealth(sources.PostgreSQL, pg, pgErr)
	result.NATS = buildNATSHealth(sources.NATS, natsStatus, natsErr, p.cfg.NATSMonitorURL != "")
	result.Dependencies = buildDependencies(sources, pg, pgErr, natsStatus, natsErr, checkedAt, result.Exchange.Venue)

	if pg.Runtime.Present {
		result.Services = append(result.Services, realService{
			Service:   "ExecutionState durable state",
			Ready:     false,
			Mode:      p.runtimeMode(),
			LastEvent: pg.Runtime.UpdatedAt,
			LagLabel:  "Persistence observed · process liveness not wired",
			Health:    "UNKNOWN",
		})
	}

	p.applyHostTelemetry(&result, checkedAt)
	if p.runtimeMode() == "PAPER" {
		result.SourceMode = "PAPER"
		result.ReadinessReason = "Paper trading uses virtual funds and simulated fills; private exchange readiness is not asserted."
		result.SourceNote = "Current public market data and simulated execution. " + result.SourceNote
		result.Exchange.Venue = "SIMULATED"
		result.Exchange.ReconnectState = "Local simulated backend; no private venue connection"
		for _, row := range result.Containers {
			if row.Name == "simulated-exchange" {
				result.Exchange.Connected = row.State == "RUNNING"
				result.Exchange.State = row.Health
			}
		}
	}
	return result
}

func buildPostgresHealth(source sourceprobe.Result, snapshot pgstore.OperationalSnapshot, queryErr error) realPostgres {
	result := realPostgres{
		Connected:         source.Reachable && queryErr == nil,
		LatencyMS:         round1(snapshot.Diagnostics.QueryLatencyMs),
		ActiveConnections: snapshot.Diagnostics.ActiveConnections,
		MaxConnections:    snapshot.Diagnostics.MaxConnections,
		StorageUsedLabel:  formatBytes(snapshot.Diagnostics.DatabaseSizeBytes),
		StoragePct:        0,
		State:             "UNKNOWN",
	}

	switch {
	case !source.Reachable:
		result.State = "CRITICAL"
		result.PersistenceState = source.Detail
	case queryErr != nil:
		result.State = "CRITICAL"
		result.PersistenceState = "TCP reachable but authenticated read failed: " + queryErr.Error()
	case snapshot.Runtime.Present:
		result.State = "HEALTHY"
		result.PersistenceState = fmt.Sprintf(
			"Runtime snapshot schema %d · updated %s · cash %.2f · positions %d · orders %d · fills %d",
			snapshot.Runtime.SchemaVersion,
			snapshot.Runtime.UpdatedAt,
			snapshot.Runtime.AccountCash,
			snapshot.Runtime.PositionCount,
			snapshot.Runtime.TrackedOrderCount,
			snapshot.Runtime.FillRows,
		)
	default:
		result.State = "HEALTHY"
		result.PersistenceState = "Authenticated read OK · trading_runtime_state not created/populated yet"
	}
	return result
}

func buildNATSHealth(source sourceprobe.Result, monitor natsdiag.MonitorStatus, monitorErr error, monitorConfigured bool) realNATS {
	result := realNATS{Connected: source.Reachable, Streams: -1, Consumers: -1, PendingMessages: -1, MaxConsumerLag: -1, Redeliveries: -1, State: "UNKNOWN"}
	switch {
	case !source.Reachable:
		result.State = "CRITICAL"
		result.AckHealthLabel = source.Detail
	case monitorConfigured && monitorErr == nil && monitor.Reachable:
		result.State = "HEALTHY"
		result.Streams = monitor.Streams
		result.Consumers = monitor.Consumers
		result.AckHealthLabel = fmt.Sprintf("JetStream monitor reachable · %d messages · %s stored", monitor.Messages, formatBytes(int64(monitor.Bytes)))
	case monitorConfigured && monitorErr != nil:
		result.State = "WARN"
		result.AckHealthLabel = "NATS TCP reachable; monitoring endpoint failed: " + monitorErr.Error()
	default:
		result.State = "WARN"
		result.AckHealthLabel = "NATS TCP reachable · JetStream monitoring URL not configured"
	}
	return result
}

func buildDependencies(
	sources sourceprobe.Status,
	pg pgstore.OperationalSnapshot,
	pgErr error,
	natsStatus natsdiag.MonitorStatus,
	natsErr error,
	checkedAt time.Time,
	executionVenue string,
) []realReadinessDependency {
	checked := checkedAt.Format("15:04:05 UTC")
	stateFromProbe := func(value sourceprobe.Result) string {
		if value.Reachable {
			return "HEALTHY"
		}
		return "CRITICAL"
	}

	postgresState := stateFromProbe(sources.PostgreSQL)
	postgresReason := sources.PostgreSQL.Detail
	if sources.PostgreSQL.Reachable && pgErr != nil {
		postgresState = "CRITICAL"
		postgresReason = "TCP reachable but authenticated read failed: " + pgErr.Error()
	} else if pg.Runtime.Present {
		postgresReason = "Authenticated read OK · durable runtime snapshot present"
	} else if sources.PostgreSQL.Reachable {
		postgresReason = "Authenticated read OK · runtime snapshot not present yet"
	}

	natsState := stateFromProbe(sources.NATS)
	natsReason := sources.NATS.Detail
	if sources.NATS.Reachable && natsErr == nil && natsStatus.Reachable {
		natsReason = fmt.Sprintf("JetStream monitor reachable · %d streams · %d consumers", natsStatus.Streams, natsStatus.Consumers)
	} else if sources.NATS.Reachable && natsErr != nil {
		natsState = "WARN"
		natsReason = "TCP reachable; monitor unavailable: " + natsErr.Error()
	}

	runtimeState := "UNKNOWN"
	runtimeReason := "Durable execution snapshot not present; start the runtime services to populate it"
	if pg.Runtime.Present {
		runtimeReason = fmt.Sprintf("Durable snapshot updated %s · last execution %s", pg.Runtime.UpdatedAt, formatBusinessTimestamp(pg.Runtime.LastExecutionTimestamp))
	}

	return []realReadinessDependency{
		{Component: "PostgreSQL", State: postgresState, Reason: postgresReason, LastCheck: checked},
		{Component: "NATS / JetStream", State: natsState, Reason: natsReason, LastCheck: checked},
		{Component: "MarketData storage", State: stateFromProbe(sources.MarketData), Reason: sources.MarketData.Detail, LastCheck: checked},
		{Component: "ExecutionState durable snapshot", State: runtimeState, Reason: runtimeReason, LastCheck: checked},
		{Component: "Trading service liveness", State: "UNKNOWN", Reason: "No common service heartbeat/readiness contract is wired yet", LastCheck: checked},
		{Component: "Reconciliation", State: "UNKNOWN", Reason: "Latest exchange snapshot/reconciliation result is not durably exposed yet", LastCheck: checked},
		{Component: "Exchange", State: "UNKNOWN", Reason: fmt.Sprintf("%s TESTNET foundation configured; public connectivity is intentionally not checked until Step 34", executionVenue), LastCheck: checked},
		{Component: "Clock sync", State: "UNKNOWN", Reason: "Host clock metrics are intentionally not inferred from the dashboard container", LastCheck: checked},
	}
}

func formatBytes(value int64) string {
	if value <= 0 {
		return "—"
	}
	const unit = 1024
	if value < unit {
		return strconv.FormatInt(value, 10) + " B"
	}
	div, exp := int64(unit), 0
	for n := value / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(value)/float64(div), "KMGTPE"[exp])
}

func formatBusinessTimestamp(value uint64) string {
	text := strconv.FormatUint(value, 10)
	if len(text) == 8 {
		return text[0:4] + "-" + text[4:6] + "-" + text[6:8]
	}
	if value == 0 {
		return "—"
	}
	return text
}

func round1(value float64) float64 {
	return math.Round(value*10) / 10
}
