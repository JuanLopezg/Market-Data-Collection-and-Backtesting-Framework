package postgres

/*
#cgo pkg-config: libpq
#include <stdlib.h>
#include <libpq-fe.h>
*/
import "C"

import (
	"context"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"

	"control-dashboard-api/internal/tradingwire"
)

const maxRecentFillLimit = 500

var ErrRuntimeStateMissing = errors.New("trading_runtime_state snapshot is missing")

type RuntimeSnapshot struct {
	State     tradingwire.TradingStateSnapshot
	UpdatedAt time.Time
}

type PersistedFill struct {
	tradingwire.Fill
	CumulativeQuantity float64
}

type FillStats struct {
	Count           uint64
	TotalCommission float64
}

// RuntimeReader is the narrow read-only boundary used by RealProvider in Step 14B.
// It exposes typed runtime state only; browser input can never become SQL.
type RuntimeReader interface {
	Health(ctx context.Context) (Health, error)
	LoadRuntimeSnapshot(ctx context.Context) (RuntimeSnapshot, error)
	LoadRecentFills(ctx context.Context, limit int) ([]PersistedFill, error)
	LoadFillStats(ctx context.Context) (FillStats, error)
	Close()
}

// Store uses libpq, the same PostgreSQL client family already used by the C++ runtime.
// The single connection is mutex-protected because dashboard reads are low-volume and
// bounded. Session default_transaction_read_only=on is asserted on every connection.
type Store struct {
	mu   sync.Mutex
	dsn  string
	conn *C.PGconn
}

func NewStore(dsn string) (*Store, error) {
	dsn = strings.TrimSpace(dsn)
	if dsn == "" {
		return nil, fmt.Errorf("PostgreSQL DSN is empty")
	}
	return &Store{dsn: hardenDSN(dsn)}, nil
}

func (s *Store) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.conn != nil {
		C.PQfinish(s.conn)
		s.conn = nil
	}
}

func (s *Store) Health(ctx context.Context) (Health, error) {
	started := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.checkContext(ctx); err != nil {
		return Health{Ready: false, Detail: err.Error()}, err
	}
	if err := s.ensureConnection(); err != nil {
		return Health{Ready: false, Latency: time.Since(started).String(), Detail: "PostgreSQL connection failed"}, err
	}

	rows, err := s.query(`
        SELECT
            current_setting('default_transaction_read_only'),
            (to_regclass('trading_runtime_state') IS NOT NULL)::text,
            (to_regclass('trading_fills') IS NOT NULL)::text
    `)
	latency := time.Since(started)
	if err != nil {
		return Health{Ready: false, Latency: latency.String(), Detail: "PostgreSQL health query failed"}, err
	}
	if len(rows) != 1 || len(rows[0]) != 3 {
		return Health{Ready: false, Latency: latency.String(), Detail: "unexpected PostgreSQL health response"}, nil
	}

	readOnly := rows[0][0] == "on"
	runtimeStatePresent := rows[0][1] == "true" || rows[0][1] == "t"
	fillsPresent := rows[0][2] == "true" || rows[0][2] == "t"
	if !readOnly || !runtimeStatePresent || !fillsPresent {
		return Health{
			Ready:   false,
			Latency: latency.String(),
			Detail: fmt.Sprintf(
				"read-only/runtime schema gate failed: read_only=%t trading_runtime_state=%t trading_fills=%t",
				readOnly,
				runtimeStatePresent,
				fillsPresent,
			),
		}, nil
	}

	return Health{Ready: true, Latency: latency.String(), Detail: "read-only runtime tables available"}, nil
}

func (s *Store) LoadRuntimeSnapshot(ctx context.Context) (RuntimeSnapshot, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.checkContext(ctx); err != nil {
		return RuntimeSnapshot{}, err
	}
	if err := s.ensureConnection(); err != nil {
		return RuntimeSnapshot{}, err
	}

	rows, err := s.query(`
        SELECT snapshot::text, EXTRACT(EPOCH FROM updated_at)::bigint::text
        FROM trading_runtime_state
        WHERE singleton=TRUE
    `)
	if err != nil {
		return RuntimeSnapshot{}, fmt.Errorf("read trading runtime snapshot: %w", err)
	}
	if len(rows) == 0 {
		return RuntimeSnapshot{}, ErrRuntimeStateMissing
	}
	if len(rows[0]) != 2 {
		return RuntimeSnapshot{}, fmt.Errorf("unexpected trading runtime snapshot shape")
	}

	state, err := tradingwire.DecodeTradingStateSnapshot([]byte(rows[0][0]))
	if err != nil {
		return RuntimeSnapshot{}, err
	}
	epoch, err := strconv.ParseInt(rows[0][1], 10, 64)
	if err != nil {
		return RuntimeSnapshot{}, fmt.Errorf("decode trading runtime updated_at: %w", err)
	}
	return RuntimeSnapshot{State: state, UpdatedAt: time.Unix(epoch, 0).UTC()}, nil
}

func (s *Store) LoadRecentFills(ctx context.Context, limit int) ([]PersistedFill, error) {
	if limit <= 0 {
		limit = 100
	}
	if limit > maxRecentFillLimit {
		limit = maxRecentFillLimit
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.checkContext(ctx); err != nil {
		return nil, err
	}
	if err := s.ensureConnection(); err != nil {
		return nil, err
	}

	// limit is internal, integer-bounded above, and never comes from browser input.
	sql := fmt.Sprintf(`
        SELECT
            fill_id::text,
            order_id::text,
            strategy_id::text,
            timestamp::text,
            coin,
            side::text,
            quantity::text,
            price::text,
            commission::text,
            SUM(quantity) OVER (
                PARTITION BY order_id
                ORDER BY timestamp, fill_id
                ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
            )::text
        FROM trading_fills
        ORDER BY timestamp DESC, fill_id DESC
        LIMIT %d
    `, limit)

	rows, err := s.query(sql)
	if err != nil {
		return nil, fmt.Errorf("read recent trading fills: %w", err)
	}

	result := make([]PersistedFill, 0, len(rows))
	for _, row := range rows {
		if len(row) != 10 {
			return nil, fmt.Errorf("unexpected trading fill row shape")
		}
		fill, err := decodeFillRow(row)
		if err != nil {
			return nil, err
		}
		result = append(result, fill)
	}
	return result, nil
}

func (s *Store) LoadFillStats(ctx context.Context) (FillStats, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.checkContext(ctx); err != nil {
		return FillStats{}, err
	}
	if err := s.ensureConnection(); err != nil {
		return FillStats{}, err
	}

	rows, err := s.query(`
        SELECT COUNT(*)::text, COALESCE(SUM(commission), 0.0)::text
        FROM trading_fills
    `)
	if err != nil {
		return FillStats{}, fmt.Errorf("read trading fill stats: %w", err)
	}
	if len(rows) != 1 || len(rows[0]) != 2 {
		return FillStats{}, fmt.Errorf("unexpected trading fill stats shape")
	}

	count, err := strconv.ParseUint(rows[0][0], 10, 64)
	if err != nil {
		return FillStats{}, fmt.Errorf("decode fill count: %w", err)
	}
	commission, err := strconv.ParseFloat(rows[0][1], 64)
	if err != nil || math.IsNaN(commission) || math.IsInf(commission, 0) {
		return FillStats{}, fmt.Errorf("decode total commission")
	}
	return FillStats{Count: count, TotalCommission: commission}, nil
}

func (s *Store) checkContext(ctx context.Context) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
		return nil
	}
}

func (s *Store) ensureConnection() error {
	if s.conn != nil && C.PQstatus(s.conn) == C.CONNECTION_OK {
		return nil
	}
	if s.conn != nil {
		C.PQfinish(s.conn)
		s.conn = nil
	}

	cDSN := C.CString(s.dsn)
	defer C.free(unsafe.Pointer(cDSN))
	conn := C.PQconnectdb(cDSN)
	if conn == nil {
		return fmt.Errorf("libpq returned a null connection")
	}
	if C.PQstatus(conn) != C.CONNECTION_OK {
		detail := strings.TrimSpace(C.GoString(C.PQerrorMessage(conn)))
		C.PQfinish(conn)
		return fmt.Errorf("PostgreSQL connection failed: %s", detail)
	}
	s.conn = conn

	// This is the critical write-safety boundary. All subsequent transactions on this
	// dashboard connection are read-only unless code explicitly overrides it; Store never does.
	if err := s.execCommand(`
        SET application_name = 'control-dashboard';
        SET default_transaction_read_only = on;
        SET statement_timeout = '2000ms';
        SET lock_timeout = '500ms';
    `); err != nil {
		C.PQfinish(s.conn)
		s.conn = nil
		return fmt.Errorf("configure read-only PostgreSQL session: %w", err)
	}
	return nil
}

func (s *Store) execCommand(sql string) error {
	cSQL := C.CString(sql)
	defer C.free(unsafe.Pointer(cSQL))
	result := C.PQexec(s.conn, cSQL)
	if result == nil {
		return fmt.Errorf("PostgreSQL command returned no result: %s", strings.TrimSpace(C.GoString(C.PQerrorMessage(s.conn))))
	}
	defer C.PQclear(result)
	if C.PQresultStatus(result) != C.PGRES_COMMAND_OK {
		return fmt.Errorf("PostgreSQL command failed: %s", strings.TrimSpace(C.GoString(C.PQresultErrorMessage(result))))
	}
	return nil
}

func (s *Store) query(sql string) ([][]string, error) {
	cSQL := C.CString(sql)
	defer C.free(unsafe.Pointer(cSQL))
	result := C.PQexec(s.conn, cSQL)
	if result == nil {
		return nil, fmt.Errorf("PostgreSQL query returned no result: %s", strings.TrimSpace(C.GoString(C.PQerrorMessage(s.conn))))
	}
	defer C.PQclear(result)

	if C.PQresultStatus(result) != C.PGRES_TUPLES_OK {
		return nil, fmt.Errorf("PostgreSQL query failed: %s", strings.TrimSpace(C.GoString(C.PQresultErrorMessage(result))))
	}

	rowCount := int(C.PQntuples(result))
	columnCount := int(C.PQnfields(result))
	rows := make([][]string, 0, rowCount)
	for row := 0; row < rowCount; row++ {
		values := make([]string, columnCount)
		for col := 0; col < columnCount; col++ {
			if C.PQgetisnull(result, C.int(row), C.int(col)) != 0 {
				values[col] = ""
				continue
			}
			values[col] = C.GoString(C.PQgetvalue(result, C.int(row), C.int(col)))
		}
		rows = append(rows, values)
	}
	return rows, nil
}

func decodeFillRow(row []string) (PersistedFill, error) {
	var result PersistedFill
	var err error
	if result.Fill.FillID, err = strconv.ParseUint(row[0], 10, 64); err != nil {
		return result, fmt.Errorf("decode fill_id: %w", err)
	}
	if result.Fill.OrderID, err = strconv.ParseUint(row[1], 10, 64); err != nil {
		return result, fmt.Errorf("decode order_id: %w", err)
	}
	if result.Fill.StrategyID, err = strconv.ParseUint(row[2], 10, 64); err != nil {
		return result, fmt.Errorf("decode strategy_id: %w", err)
	}
	if result.Fill.Timestamp, err = strconv.ParseUint(row[3], 10, 64); err != nil {
		return result, fmt.Errorf("decode fill timestamp: %w", err)
	}
	result.Fill.Coin = row[4]
	if result.Fill.Side, err = strconv.Atoi(row[5]); err != nil {
		return result, fmt.Errorf("decode fill side: %w", err)
	}
	if result.Fill.Quantity, err = strconv.ParseFloat(row[6], 64); err != nil {
		return result, fmt.Errorf("decode fill quantity: %w", err)
	}
	if result.Fill.Price, err = strconv.ParseFloat(row[7], 64); err != nil {
		return result, fmt.Errorf("decode fill price: %w", err)
	}
	if result.Fill.Commission, err = strconv.ParseFloat(row[8], 64); err != nil {
		return result, fmt.Errorf("decode fill commission: %w", err)
	}
	if result.CumulativeQuantity, err = strconv.ParseFloat(row[9], 64); err != nil {
		return result, fmt.Errorf("decode cumulative fill quantity: %w", err)
	}
	if result.Fill.Coin == "" || result.Fill.Quantity <= 0 || result.Fill.Price <= 0 || result.Fill.Commission < 0 {
		return result, fmt.Errorf("invalid persisted fill row")
	}
	return result, nil
}

func hardenDSN(dsn string) string {
	lower := strings.ToLower(dsn)
	if strings.HasPrefix(lower, "postgres://") || strings.HasPrefix(lower, "postgresql://") {
		separator := "?"
		if strings.Contains(dsn, "?") {
			separator = "&"
		}
		if !strings.Contains(lower, "connect_timeout=") {
			dsn += separator + "connect_timeout=2"
			separator = "&"
		}
		if !strings.Contains(lower, "tcp_user_timeout=") {
			dsn += separator + "tcp_user_timeout=2000"
		}
		return dsn
	}
	if !strings.Contains(lower, "connect_timeout=") {
		dsn += " connect_timeout=2"
	}
	if !strings.Contains(lower, "tcp_user_timeout=") {
		dsn += " tcp_user_timeout=2000"
	}
	return dsn
}
