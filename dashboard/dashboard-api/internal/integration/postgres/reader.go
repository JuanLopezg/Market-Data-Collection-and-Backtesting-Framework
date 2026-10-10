package postgres

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"control-dashboard-api/internal/tradingwire"
)

// QueryRunner is intentionally tiny so the PostgreSQL read layer can be unit
// tested without a live database. The production implementation shells out to
// the official psql client shipped in the dashboard-api image. This keeps the
// Go binary dependency-free while still using libpq/SCRAM authentication.
type QueryRunner interface {
	QueryJSON(ctx context.Context, dsn, query string) ([]byte, error)
}

type PSQLRunner struct{}

func (PSQLRunner) QueryJSON(ctx context.Context, dsn, query string) ([]byte, error) {
	env, err := postgresEnv(dsn)
	if err != nil {
		return nil, err
	}

	cmd := exec.CommandContext(ctx, "psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=1", "-c", query)
	cmd.Env = append(os.Environ(), env...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		detail := strings.TrimSpace(string(output))
		if detail == "" {
			detail = err.Error()
		}
		if len(detail) > 700 {
			detail = detail[len(detail)-700:]
		}
		return nil, fmt.Errorf("read-only PostgreSQL query failed: %s", detail)
	}

	raw := strings.TrimSpace(string(output))
	if raw == "" {
		return nil, nil
	}
	if !json.Valid([]byte(raw)) {
		return nil, fmt.Errorf("PostgreSQL query returned non-JSON output")
	}
	return []byte(raw), nil
}

type Config struct {
	DSN     string
	Timeout time.Duration
	Runner  QueryRunner
}

type Reader struct {
	dsn     string
	timeout time.Duration
	runner  QueryRunner
}

type TablePresence struct {
	RuntimeState       bool `json:"runtimeState"`
	TradingFills       bool `json:"tradingFills"`
	StrategyCheckpoint bool `json:"strategyCheckpoint"`
	RiskCheckpoint     bool `json:"riskCheckpoint"`
	PlannerCheckpoint  bool `json:"plannerCheckpoint"`
}

type Diagnostics struct {
	ActiveConnections int           `json:"activeConnections"`
	MaxConnections    int           `json:"maxConnections"`
	DatabaseSizeBytes int64         `json:"databaseSizeBytes"`
	Tables            TablePresence `json:"tables"`
	QueryLatencyMs    float64       `json:"queryLatencyMs"`
}

type RuntimeSummary struct {
	Present                bool    `json:"present"`
	SchemaVersion          uint32  `json:"schemaVersion"`
	UpdatedAt              string  `json:"updatedAt"`
	LastBarCloseTimestamp  uint64  `json:"lastBarCloseTimestamp"`
	LastExecutionTimestamp uint64  `json:"lastExecutionTimestamp"`
	AccountCash            float64 `json:"accountCash"`
	PositionCount          int     `json:"positionCount"`
	StrategyCount          int     `json:"strategyCount"`
	PendingPlanCount       int     `json:"pendingPlanCount"`
	TrackedOrderCount      int     `json:"trackedOrderCount"`
	ProcessedFillCount     int     `json:"processedFillCount"`
	FillRows               int64   `json:"fillRows"`
	LatestFillTimestamp    uint64  `json:"latestFillTimestamp"`
}

type OperationalSnapshot struct {
	Diagnostics Diagnostics    `json:"diagnostics"`
	Runtime     RuntimeSummary `json:"runtime"`
}

// FillWindow is a bounded read-only projection of persisted fills. TotalRows
// counts the full durable table while Rows is limited to the tracked orders
// requested by the caller. TotalCommission is durable across the full table.
type FillWindow struct {
	TotalRows       int64              `json:"totalRows"`
	TotalCommission float64            `json:"totalCommission"`
	Rows            []tradingwire.Fill `json:"rows"`
}

// LedgerFillWindow is the bounded Step 42 accounting foundation read. The
// aggregate fields cover the complete append-only trading_fills table while
// Rows carries only a recent deterministic window for operator inspection.
type LedgerFillWindow struct {
	TotalRows           int64              `json:"totalRows"`
	DistinctFillIDs     int64              `json:"distinctFillIds"`
	InvalidRows         int64              `json:"invalidRows"`
	TotalCommission     float64            `json:"totalCommission"`
	GrossBuyNotional    float64            `json:"grossBuyNotional"`
	GrossSellNotional   float64            `json:"grossSellNotional"`
	LatestFillID        uint64             `json:"latestFillId"`
	LatestFillTimestamp uint64             `json:"latestFillTimestamp"`
	Rows                []tradingwire.Fill `json:"rows"`
}

// RuntimeState is the durable execution-state snapshot used by read-only
// dashboard projections. UpdatedAt is the PostgreSQL persistence timestamp;
// Snapshot mirrors the verified trading runtime contract.
type RuntimeState struct {
	UpdatedAt string                           `json:"updatedAt"`
	Snapshot  tradingwire.TradingStateSnapshot `json:"snapshot"`
}

// PipelineCheckpoint is a single, cycle-aligned durable lineage anchored on the
// latest PortfolioRisk decision. Optional Strategy/Planner payloads are joined
// only at the exact same business timestamp; the reader never mixes cycles.
type PipelineCheckpoint struct {
	RiskDiagnostics *RiskDiagnostics
	Timestamp       uint64
	PortfolioConfig string
	StrategyUpdate  *tradingwire.MarketDataUpdated
	Signals         tradingwire.StrategyIntentBatch
	Account         tradingwire.AccountSnapshot
	Decision        tradingwire.DecisionBatch
	PlanningRequest *tradingwire.NotionalOrderPlanningRequest
	Plan            *tradingwire.NotionalOrderPlanBatch
}

// StrategyCheckpoint is the latest durable output of StrategyService paired
// with the exact MarketDataUpdated payload that triggered it.
type StrategyCheckpoint struct {
	Timestamp uint64
	Update    tradingwire.MarketDataUpdated
	Intents   tradingwire.StrategyIntentBatch
}

func NewReader(cfg Config) *Reader {
	if cfg.Timeout <= 0 {
		cfg.Timeout = 2 * time.Second
	}
	if cfg.Runner == nil {
		cfg.Runner = PSQLRunner{}
	}
	return &Reader{dsn: strings.TrimSpace(cfg.DSN), timeout: cfg.Timeout, runner: cfg.Runner}
}

func (r *Reader) Snapshot(ctx context.Context) (OperationalSnapshot, error) {
	var result OperationalSnapshot
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}

	started := time.Now()
	raw, err := r.query(ctx, diagnosticsSQL)
	if err != nil {
		return result, err
	}
	if err := json.Unmarshal(raw, &result.Diagnostics); err != nil {
		return result, fmt.Errorf("decode PostgreSQL diagnostics: %w", err)
	}
	result.Diagnostics.QueryLatencyMs = float64(time.Since(started).Microseconds()) / 1000.0

	if result.Diagnostics.Tables.RuntimeState {
		raw, err = r.query(ctx, runtimeSummarySQL)
		if err != nil {
			return result, err
		}
		if len(raw) != 0 && string(raw) != "null" {
			if err := json.Unmarshal(raw, &result.Runtime); err != nil {
				return result, fmt.Errorf("decode trading runtime summary: %w", err)
			}
			result.Runtime.Present = true
		}
	}

	if result.Diagnostics.Tables.TradingFills {
		raw, err = r.query(ctx, fillSummarySQL)
		if err != nil {
			return result, err
		}
		if len(raw) != 0 && string(raw) != "null" {
			var fills struct {
				FillRows            int64  `json:"fillRows"`
				LatestFillTimestamp uint64 `json:"latestFillTimestamp"`
			}
			if err := json.Unmarshal(raw, &fills); err != nil {
				return result, fmt.Errorf("decode trading fill summary: %w", err)
			}
			result.Runtime.FillRows = fills.FillRows
			result.Runtime.LatestFillTimestamp = fills.LatestFillTimestamp
		}
	}

	return result, nil
}

func (r *Reader) RuntimeState(ctx context.Context) (RuntimeState, error) {
	var result RuntimeState
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}

	raw, err := r.query(ctx, runtimeStateSQL)
	if err != nil {
		return result, err
	}
	if len(raw) == 0 || string(raw) == "null" {
		return result, ErrRuntimeStateNotPresent
	}

	var envelope struct {
		UpdatedAt string          `json:"updatedAt"`
		Snapshot  json.RawMessage `json:"snapshot"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return result, fmt.Errorf("decode trading runtime state envelope: %w", err)
	}
	if len(envelope.Snapshot) == 0 || string(envelope.Snapshot) == "null" {
		return result, fmt.Errorf("trading runtime state contains no snapshot")
	}

	snapshot, err := tradingwire.DecodeTradingStateSnapshot(envelope.Snapshot)
	if err != nil {
		return result, err
	}
	result.UpdatedAt = envelope.UpdatedAt
	result.Snapshot = snapshot
	return result, nil
}

var ErrRuntimeStateNotPresent = errors.New("trading runtime state is not present")

// FillsForOrders returns persisted fills only for the supplied numeric order IDs.
// The result is intentionally bounded and read-only; global count/commission are
// returned as aggregate diagnostics without loading the entire fill ledger.

// LatestPipelineCheckpoint reconstructs one durable normal-cycle lineage. The
// PortfolioRisk checkpoint is the anchor because it durably stores the exact
// StrategyIntentBatch and AccountSnapshot that produced the DecisionBatch.
// Strategy and OrderPlanner payloads are joined only when their timestamp is
// identical to that decision timestamp.
func (r *Reader) LatestPipelineCheckpoint(ctx context.Context) (PipelineCheckpoint, error) {
	var result PipelineCheckpoint
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}
	raw, err := r.query(ctx, pipelineCheckpointSQL)
	if err != nil {
		return result, err
	}
	if len(raw) == 0 || string(raw) == "null" {
		return result, ErrPipelineCheckpointNotPresent
	}
	var envelope struct {
		Timestamp              uint64  `json:"timestamp"`
		PortfolioConfig        string  `json:"portfolioConfig"`
		RiskDiagnosticsPayload *string `json:"riskDiagnosticsPayload"`
		StrategyUpdatePayload  *string `json:"strategyUpdatePayload"`
		SignalsPayload         string  `json:"signalsPayload"`
		AccountPayload         string  `json:"accountPayload"`
		DecisionPayload        string  `json:"decisionPayload"`
		PlanningRequestPayload *string `json:"planningRequestPayload"`
		PlanPayload            *string `json:"planPayload"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return result, fmt.Errorf("decode pipeline checkpoint envelope: %w", err)
	}
	if envelope.Timestamp == 0 {
		return result, fmt.Errorf("pipeline checkpoint has zero timestamp")
	}
	signals, err := tradingwire.DecodeStrategyIntentBatch([]byte(envelope.SignalsPayload))
	if err != nil {
		return result, err
	}
	account, err := tradingwire.DecodeAccountSnapshot([]byte(envelope.AccountPayload))
	if err != nil {
		return result, err
	}
	decision, err := tradingwire.DecodeDecisionBatch([]byte(envelope.DecisionPayload))
	if err != nil {
		return result, err
	}
	if signals.Timestamp != envelope.Timestamp || account.Timestamp != envelope.Timestamp || decision.DecisionTimestamp != envelope.Timestamp {
		return result, fmt.Errorf("pipeline checkpoint payload timestamps do not match anchor %d", envelope.Timestamp)
	}
	result.Timestamp = envelope.Timestamp
	result.PortfolioConfig = envelope.PortfolioConfig
	result.Signals = signals
	result.Account = account
	result.Decision = decision
	if envelope.RiskDiagnosticsPayload != nil && *envelope.RiskDiagnosticsPayload != "" {
		result.RiskDiagnostics, err = decodeRiskDiagnostics(*envelope.RiskDiagnosticsPayload, envelope.Timestamp, envelope.PortfolioConfig)
		if err != nil {
			return PipelineCheckpoint{}, err
		}
		if len(result.RiskDiagnostics.Strategies) != len(signals.Strategies) {
			return PipelineCheckpoint{}, fmt.Errorf("risk diagnostics strategy count does not match cycle inputs")
		}
		for _, evaluated := range result.RiskDiagnostics.Strategies {
			matched := false
			for _, input := range signals.Strategies {
				matched = matched || (input.StrategyID == evaluated.StrategyID && input.StrategyName == evaluated.Name)
			}
			if !matched {
				return PipelineCheckpoint{}, fmt.Errorf("risk diagnostics strategy does not match cycle inputs")
			}
		}
	}

	if envelope.StrategyUpdatePayload != nil && strings.TrimSpace(*envelope.StrategyUpdatePayload) != "" {
		value, err := tradingwire.DecodeMarketDataUpdated([]byte(*envelope.StrategyUpdatePayload))
		if err != nil {
			return result, err
		}
		if value.CompletedThrough != envelope.Timestamp {
			return result, fmt.Errorf("strategy update timestamp does not match pipeline anchor")
		}
		result.StrategyUpdate = &value
	}
	if envelope.PlanningRequestPayload != nil && strings.TrimSpace(*envelope.PlanningRequestPayload) != "" {
		value, err := tradingwire.DecodeNotionalPlanningRequest([]byte(*envelope.PlanningRequestPayload))
		if err != nil {
			return result, err
		}
		if value.DecisionTimestamp != envelope.Timestamp {
			return result, fmt.Errorf("planning request timestamp does not match pipeline anchor")
		}
		result.PlanningRequest = &value
	}
	if envelope.PlanPayload != nil && strings.TrimSpace(*envelope.PlanPayload) != "" {
		value, err := tradingwire.DecodeNotionalOrderPlan([]byte(*envelope.PlanPayload))
		if err != nil {
			return result, err
		}
		if value.DecisionTimestamp != envelope.Timestamp {
			return result, fmt.Errorf("order plan timestamp does not match pipeline anchor")
		}
		result.Plan = &value
	}
	return result, nil
}

var ErrPipelineCheckpointNotPresent = errors.New("pipeline checkpoint is not present")

func (r *Reader) LatestStrategyCheckpoint(ctx context.Context) (StrategyCheckpoint, error) {
	var result StrategyCheckpoint
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}
	raw, err := r.query(ctx, strategyCheckpointSQL)
	if err != nil {
		return result, err
	}
	if len(raw) == 0 || string(raw) == "null" {
		return result, ErrStrategyCheckpointNotPresent
	}
	var envelope struct {
		Timestamp     uint64 `json:"timestamp"`
		UpdatePayload string `json:"updatePayload"`
		IntentPayload string `json:"intentPayload"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return result, fmt.Errorf("decode strategy checkpoint envelope: %w", err)
	}
	update, err := tradingwire.DecodeMarketDataUpdated([]byte(envelope.UpdatePayload))
	if err != nil {
		return result, err
	}
	intents, err := tradingwire.DecodeStrategyIntentBatch([]byte(envelope.IntentPayload))
	if err != nil {
		return result, err
	}
	if envelope.Timestamp == 0 || update.CompletedThrough != envelope.Timestamp || intents.Timestamp != envelope.Timestamp {
		return result, fmt.Errorf("strategy checkpoint payload timestamps do not match anchor %d", envelope.Timestamp)
	}
	result.Timestamp = envelope.Timestamp
	result.Update = update
	result.Intents = intents
	return result, nil
}

var ErrStrategyCheckpointNotPresent = errors.New("strategy checkpoint is not present")

// LedgerFills returns full-table economic aggregates plus a bounded recent
// append-only fill window. The query is fixed server-side and read-only.
func (r *Reader) LedgerFills(ctx context.Context, limit int) (LedgerFillWindow, error) {
	var result LedgerFillWindow
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}
	if limit <= 0 {
		limit = 250
	}
	if limit > 1000 {
		limit = 1000
	}
	raw, err := r.query(ctx, fmt.Sprintf(ledgerFillsSQL, limit))
	if err != nil {
		return result, err
	}
	if len(raw) == 0 || string(raw) == "null" {
		result.Rows = []tradingwire.Fill{}
		return result, nil
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return result, fmt.Errorf("decode Step 42 ledger fill window: %w", err)
	}
	if result.Rows == nil {
		result.Rows = []tradingwire.Fill{}
	}
	return result, nil
}

func (r *Reader) FillsForOrders(ctx context.Context, orderIDs []uint64, limit int) (FillWindow, error) {
	var result FillWindow
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}
	if limit <= 0 {
		limit = 1000
	}
	if limit > 2000 {
		limit = 2000
	}

	ids := make([]string, 0, len(orderIDs))
	seen := make(map[uint64]struct{}, len(orderIDs))
	for _, id := range orderIDs {
		if id == 0 {
			continue
		}
		if _, exists := seen[id]; exists {
			continue
		}
		seen[id] = struct{}{}
		ids = append(ids, strconv.FormatUint(id, 10))
	}

	where := "FALSE"
	if len(ids) > 0 {
		where = "order_id IN (" + strings.Join(ids, ",") + ")"
	}
	query := fmt.Sprintf(fillsForOrdersSQL, where, limit)
	raw, err := r.query(ctx, query)
	if err != nil {
		return result, err
	}
	if len(raw) == 0 || string(raw) == "null" {
		return result, nil
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return result, fmt.Errorf("decode trading fills window: %w", err)
	}
	if result.Rows == nil {
		result.Rows = []tradingwire.Fill{}
	}
	return result, nil
}

func (r *Reader) query(parent context.Context, query string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(parent, r.timeout)
	defer cancel()
	raw, err := r.runner.QueryJSON(ctx, r.dsn, query)
	if err != nil {
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			return nil, fmt.Errorf("PostgreSQL query timed out after %s", r.timeout)
		}
		return nil, err
	}
	return raw, nil
}

const diagnosticsSQL = `SELECT json_build_object(
  'activeConnections', (SELECT count(*) FROM pg_stat_activity WHERE datname = current_database()),
  'maxConnections', current_setting('max_connections')::int,
  'databaseSizeBytes', pg_database_size(current_database()),
  'tables', json_build_object(
    'runtimeState', to_regclass('public.trading_runtime_state') IS NOT NULL,
    'tradingFills', to_regclass('public.trading_fills') IS NOT NULL,
    'strategyCheckpoint', to_regclass('public.strategy_market_update_checkpoint') IS NOT NULL,
    'riskCheckpoint', to_regclass('public.portfolio_risk_live_decision_checkpoint') IS NOT NULL,
    'plannerCheckpoint', to_regclass('public.order_planner_live_notional_checkpoint') IS NOT NULL
  )
)::text;`

const runtimeSummarySQL = `SELECT json_build_object(
  'schemaVersion', schema_version,
  'updatedAt', to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
  'lastBarCloseTimestamp', COALESCE((snapshot->>'last_bar_close_timestamp')::bigint, 0),
  'lastExecutionTimestamp', COALESCE((snapshot->>'last_execution_timestamp')::bigint, 0),
  'accountCash', COALESCE((snapshot->>'account_cash')::double precision, 0),
  'positionCount', CASE
    WHEN jsonb_typeof(snapshot->'account_positions') = 'object'
      THEN (SELECT count(*) FROM jsonb_object_keys(snapshot->'account_positions'))
    ELSE 0
  END,
  'strategyCount', CASE
    WHEN jsonb_typeof(snapshot->'strategies') = 'array'
      THEN jsonb_array_length(snapshot->'strategies')
    ELSE 0
  END,
  'pendingPlanCount', CASE
    WHEN jsonb_typeof(snapshot->'pending_plans') = 'array'
      THEN jsonb_array_length(snapshot->'pending_plans')
    ELSE 0
  END,
  'trackedOrderCount', CASE
    WHEN jsonb_typeof(snapshot->'orders') = 'array'
      THEN jsonb_array_length(snapshot->'orders')
    ELSE 0
  END,
  'processedFillCount', CASE
    WHEN jsonb_typeof(snapshot->'processed_fill_ids') = 'array'
      THEN jsonb_array_length(snapshot->'processed_fill_ids')
    ELSE 0
  END
)::text
FROM trading_runtime_state WHERE singleton = TRUE;`

const runtimeStateSQL = `SELECT json_build_object(
  'updatedAt', to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
  'snapshot', snapshot
)::text
FROM trading_runtime_state WHERE singleton = TRUE;`

const fillSummarySQL = `SELECT json_build_object(
  'fillRows', count(*),
  'latestFillTimestamp', COALESCE(max(timestamp), 0)
)::text FROM trading_fills;`

const pipelineCheckpointSQL = `WITH risk AS (
  SELECT timestamp, signals_payload, account_payload, decision_payload,
    to_jsonb(checkpoint)->>'risk_diagnostics_payload' AS risk_diagnostics_payload
  FROM portfolio_risk_live_decision_checkpoint checkpoint
  WHERE state_key = 'portfolio-risk-live-sqlite-v1'
  ORDER BY timestamp DESC
  LIMIT 1
)
SELECT json_build_object(
  'timestamp', risk.timestamp,
  'portfolioConfig', (
    SELECT portfolio_config FROM portfolio_risk_service_metadata
    WHERE state_key = 'portfolio-risk-live-sqlite-v1' LIMIT 1
  ),
  'strategyUpdatePayload', (
    SELECT update_payload FROM strategy_market_update_checkpoint
    WHERE state_key = 'strategy-service-market-db-v1' AND timestamp = risk.timestamp
    LIMIT 1
  ),
  'signalsPayload', risk.signals_payload,
  'accountPayload', risk.account_payload,
  'decisionPayload', risk.decision_payload,
  'riskDiagnosticsPayload', risk.risk_diagnostics_payload,
  'planningRequestPayload', (
    SELECT request_payload FROM order_planner_live_notional_checkpoint
    WHERE state_key = 'order-planner-live-notional-v1' AND timestamp = risk.timestamp
    LIMIT 1
  ),
  'planPayload', (
    SELECT plan_payload FROM order_planner_live_notional_checkpoint
    WHERE state_key = 'order-planner-live-notional-v1' AND timestamp = risk.timestamp
    LIMIT 1
  )
)::text FROM risk;`

const strategyCheckpointSQL = `SELECT json_build_object(
  'timestamp', timestamp,
  'updatePayload', update_payload,
  'intentPayload', intent_payload
)::text
FROM strategy_market_update_checkpoint
WHERE state_key = 'strategy-service-market-db-v1'
ORDER BY timestamp DESC
LIMIT 1;`

const ledgerFillsSQL = `SELECT json_build_object(
  'totalRows', count(*),
  'distinctFillIds', count(DISTINCT fill_id),
  'invalidRows', count(*) FILTER (WHERE fill_id IS NULL OR fill_id <= 0 OR order_id IS NULL OR order_id <= 0 OR strategy_id IS NULL OR strategy_id <= 0 OR timestamp IS NULL OR timestamp <= 0 OR coin IS NULL OR btrim(coin) = '' OR side IS NULL OR side NOT IN (0,1) OR quantity IS NULL OR quantity <= 0 OR price IS NULL OR price <= 0 OR commission IS NULL OR commission < 0),
  'totalCommission', COALESCE(sum(commission), 0),
  'grossBuyNotional', COALESCE(sum(CASE WHEN side = 0 THEN quantity * price ELSE 0 END), 0),
  'grossSellNotional', COALESCE(sum(CASE WHEN side = 1 THEN quantity * price ELSE 0 END), 0),
  'latestFillId', COALESCE(max(fill_id), 0),
  'latestFillTimestamp', COALESCE(max(timestamp), 0),
  'rows', COALESCE((
    SELECT json_agg(row_to_json(f) ORDER BY f.timestamp, f.fill_id)
    FROM (
      SELECT fill_id, order_id, strategy_id, timestamp, coin, side, quantity, price, commission
      FROM trading_fills
      ORDER BY timestamp DESC, fill_id DESC
      LIMIT %d
    ) AS f
  ), '[]'::json)
)::text FROM trading_fills;`

const fillsForOrdersSQL = `SELECT json_build_object(
  'totalRows', (SELECT count(*) FROM trading_fills),
  'totalCommission', COALESCE((SELECT sum(commission) FROM trading_fills), 0),
  'rows', COALESCE((
    SELECT json_agg(row_to_json(f) ORDER BY f.timestamp, f.fill_id)
    FROM (
      SELECT fill_id, order_id, strategy_id, timestamp, coin, side, quantity, price, commission
      FROM trading_fills
      WHERE %s
      ORDER BY timestamp DESC, fill_id DESC
      LIMIT %d
    ) AS f
  ), '[]'::json)
)::text;`

func postgresEnv(dsn string) ([]string, error) {
	values, err := parseDSN(dsn)
	if err != nil {
		return nil, err
	}
	host := values["host"]
	if host == "" {
		host = "localhost"
	}
	port := values["port"]
	if port == "" {
		port = "5432"
	}
	database := values["dbname"]
	if database == "" {
		database = values["database"]
	}
	if database == "" {
		database = "postgres"
	}
	user := values["user"]
	if user == "" {
		return nil, errors.New("PostgreSQL DSN is missing user")
	}

	env := []string{
		"PGHOST=" + host,
		"PGPORT=" + port,
		"PGDATABASE=" + database,
		"PGUSER=" + user,
		"PGCONNECT_TIMEOUT=2",
		"PGAPPNAME=control-dashboard-readonly",
		"PGOPTIONS=-c default_transaction_read_only=on -c statement_timeout=1500",
	}
	if password := values["password"]; password != "" {
		env = append(env, "PGPASSWORD="+password)
	}
	if sslmode := values["sslmode"]; sslmode != "" {
		env = append(env, "PGSSLMODE="+sslmode)
	}
	return env, nil
}

func parseDSN(dsn string) (map[string]string, error) {
	dsn = strings.TrimSpace(dsn)
	if dsn == "" {
		return nil, errors.New("PostgreSQL DSN is empty")
	}
	if strings.Contains(dsn, "://") {
		parsed, err := url.Parse(dsn)
		if err != nil || parsed.Hostname() == "" {
			return nil, errors.New("invalid PostgreSQL URL DSN")
		}
		result := map[string]string{
			"host":   parsed.Hostname(),
			"port":   parsed.Port(),
			"dbname": strings.TrimPrefix(parsed.Path, "/"),
		}
		if parsed.User != nil {
			result["user"] = parsed.User.Username()
			if password, ok := parsed.User.Password(); ok {
				result["password"] = password
			}
		}
		query := parsed.Query()
		if value := query.Get("sslmode"); value != "" {
			result["sslmode"] = value
		}
		return result, nil
	}

	result := make(map[string]string)
	for _, field := range strings.Fields(dsn) {
		key, value, ok := strings.Cut(field, "=")
		if !ok {
			continue
		}
		result[strings.TrimSpace(key)] = strings.Trim(strings.TrimSpace(value), "'\"")
	}
	if len(result) == 0 {
		return nil, errors.New("invalid PostgreSQL key/value DSN")
	}
	if port := result["port"]; port != "" {
		if _, err := strconv.Atoi(port); err != nil {
			return nil, errors.New("invalid PostgreSQL port")
		}
	}
	return result, nil
}
