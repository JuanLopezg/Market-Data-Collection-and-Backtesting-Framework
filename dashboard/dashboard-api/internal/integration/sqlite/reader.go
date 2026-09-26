package sqlite

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"time"
)

// QueryRunner keeps the market-data reader testable without linking a SQLite
// driver into the dashboard binary. The production runner delegates to the
// distro sqlite3 client in strict read-only mode.
type QueryRunner interface {
	QueryJSON(ctx context.Context, databasePath, query string) ([]byte, error)
}

type CLIRunner struct{}

func sqliteCLIArgs(databasePath, query string) []string {
	return []string{
		"-readonly",
		"-json",
		"-cmd", "PRAGMA query_only=ON;",
		// Use the sqlite3 shell dot-command instead of PRAGMA busy_timeout.
		// `PRAGMA busy_timeout=...` emits its numeric value on stdout, which
		// corrupts the single JSON document expected by QueryJSON. `.timeout`
		// configures the same busy wait without adding rows to stdout.
		"-cmd", ".timeout 1000",
		"-cmd", "PRAGMA temp_store=MEMORY;",
		databasePath,
		query,
	}
}

func (CLIRunner) QueryJSON(ctx context.Context, databasePath, query string) ([]byte, error) {
	if strings.TrimSpace(databasePath) == "" {
		return nil, errors.New("market-data SQLite path is empty")
	}
	cmd := exec.CommandContext(ctx, "sqlite3", sqliteCLIArgs(databasePath, query)...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		message := strings.TrimSpace(string(output))
		if message == "" {
			message = err.Error()
		}
		return nil, fmt.Errorf("read-only SQLite query failed: %s", message)
	}
	return []byte(strings.TrimSpace(string(output))), nil
}

type ReaderConfig struct {
	DatabasePath string
	Timeout      time.Duration
	Runner       QueryRunner
}

type Reader struct {
	databasePath string
	timeout      time.Duration
	runner       QueryRunner
}

type RankingRow struct {
	Rank        int     `json:"rank"`
	Pair        string  `json:"pair"`
	QuoteVolume float64 `json:"quote_volume"`
}

type Bar struct {
	Pair   string  `json:"pair"`
	Date   uint64  `json:"date"`
	Open   float64 `json:"open"`
	High   float64 `json:"high"`
	Low    float64 `json:"low"`
	Close  float64 `json:"close"`
	Volume float64 `json:"volume"`
}

type Integrity struct {
	DuplicateRows int `json:"duplicateRows"`
	InvalidRows   int `json:"invalidRows"`
}

type StrategyWindow struct {
	LatestDate uint64       `json:"latestDate"`
	StartDate  uint64       `json:"startDate"`
	Ranking    []RankingRow `json:"ranking"`
	Bars       []Bar        `json:"bars"`
	Integrity  Integrity    `json:"integrity"`
}

func NewReader(cfg ReaderConfig) *Reader {
	if cfg.Timeout <= 0 {
		cfg.Timeout = 2 * time.Second
	}
	if cfg.Runner == nil {
		cfg.Runner = CLIRunner{}
	}
	return &Reader{databasePath: strings.TrimSpace(cfg.DatabasePath), timeout: cfg.Timeout, runner: cfg.Runner}
}

// LoadStrategyWindow mirrors the bounded canonical strategy read: latest
// persisted ranking date, current exchange top-N, and only the configured
// calendar-history window. It never writes to the market database.
func (r *Reader) LoadStrategyWindow(ctx context.Context, canonicalTopN, historyDays int) (StrategyWindow, error) {
	var result StrategyWindow
	if r.databasePath == "" {
		return result, errors.New("market-data SQLite path is not configured")
	}
	if canonicalTopN <= 0 || canonicalTopN > 500 {
		return result, errors.New("canonical top-N must be between 1 and 500")
	}
	if historyDays <= 0 || historyDays > 1500 {
		return result, errors.New("history days must be between 1 and 1500")
	}

	var latestRows []struct {
		LatestDate uint64 `json:"latest_date"`
	}
	if err := r.queryRows(ctx, `SELECT COALESCE(MAX(date), 0) AS latest_date FROM market_volume_rank_daily;`, &latestRows); err != nil {
		return result, err
	}
	if len(latestRows) != 1 || latestRows[0].LatestDate == 0 {
		return result, errors.New("canonical market-data database has no persisted ranking date")
	}
	result.LatestDate = latestRows[0].LatestDate

	startDate, err := subtractCalendarDays(result.LatestDate, historyDays-1)
	if err != nil {
		return result, err
	}
	result.StartDate = startDate

	rankingSQL := fmt.Sprintf(
		`SELECT rank, pair, quote_volume FROM market_volume_rank_daily WHERE date = %d AND rank <= %d ORDER BY rank ASC;`,
		result.LatestDate, canonicalTopN,
	)
	if err := r.queryRows(ctx, rankingSQL, &result.Ranking); err != nil {
		return result, err
	}
	if len(result.Ranking) == 0 {
		return result, fmt.Errorf("canonical market-data database has no ranking rows for %d", result.LatestDate)
	}

	symbols := make([]string, 0, len(result.Ranking))
	seen := make(map[string]struct{}, len(result.Ranking))
	for _, row := range result.Ranking {
		pair := strings.TrimSpace(row.Pair)
		if pair == "" {
			return result, errors.New("canonical market-data ranking contains an empty pair")
		}
		if _, ok := seen[pair]; ok {
			return result, fmt.Errorf("canonical market-data ranking contains duplicate pair %q", pair)
		}
		seen[pair] = struct{}{}
		symbols = append(symbols, pair)
	}

	inList := quotedStringList(symbols)
	barsSQL := fmt.Sprintf(
		`SELECT pair, date, open, high, low, close, volume FROM ohlcv_data WHERE date BETWEEN %d AND %d AND pair IN (%s) ORDER BY pair ASC, date ASC;`,
		result.StartDate, result.LatestDate, inList,
	)
	if err := r.queryRows(ctx, barsSQL, &result.Bars); err != nil {
		return result, err
	}

	var integrityRows []Integrity
	integritySQL := fmt.Sprintf(`SELECT
  (SELECT COUNT(*) FROM (
    SELECT pair, date FROM ohlcv_data
    WHERE date BETWEEN %d AND %d AND pair IN (%s)
    GROUP BY pair, date HAVING COUNT(*) > 1
  )) AS duplicateRows,
  (SELECT COUNT(*) FROM ohlcv_data
    WHERE date BETWEEN %d AND %d AND pair IN (%s)
      AND (
        open <= 0 OR high <= 0 OR low <= 0 OR close <= 0 OR volume < 0 OR
        high < max(open, close, low) OR low > min(open, close, high)
      )
  ) AS invalidRows;`,
		result.StartDate, result.LatestDate, inList,
		result.StartDate, result.LatestDate, inList,
	)
	if err := r.queryRows(ctx, integritySQL, &integrityRows); err != nil {
		return result, err
	}
	if len(integrityRows) == 1 {
		result.Integrity = integrityRows[0]
	}
	return result, nil
}

func (r *Reader) queryRows(parent context.Context, query string, destination any) error {
	ctx, cancel := context.WithTimeout(parent, r.timeout)
	defer cancel()
	raw, err := r.runner.QueryJSON(ctx, r.databasePath, query)
	if err != nil {
		if errors.Is(ctx.Err(), context.DeadlineExceeded) {
			return fmt.Errorf("SQLite query timed out after %s", r.timeout)
		}
		return err
	}
	if len(raw) == 0 {
		raw = []byte(`[]`)
	}
	if err := json.Unmarshal(raw, destination); err != nil {
		return fmt.Errorf("decode SQLite JSON result: %w", err)
	}
	return nil
}

func quotedStringList(values []string) string {
	copyValues := append([]string(nil), values...)
	sort.Strings(copyValues)
	parts := make([]string, 0, len(copyValues))
	for _, value := range copyValues {
		parts = append(parts, "'"+strings.ReplaceAll(value, "'", "''")+"'")
	}
	return strings.Join(parts, ",")
}

func subtractCalendarDays(value uint64, days int) (uint64, error) {
	text := fmt.Sprintf("%08d", value)
	parsed, err := time.Parse("20060102", text)
	if err != nil {
		return 0, fmt.Errorf("invalid YYYYMMDD market date %d: %w", value, err)
	}
	return uint64(parseIntDate(parsed.AddDate(0, 0, -days))), nil
}

func parseIntDate(value time.Time) int {
	parsed, _ := strconv.Atoi(value.UTC().Format("20060102"))
	return parsed
}

// DailyRankingRow is a bounded canonical liquidity-ranking observation used by
// the Live-vs-Expected rolling baseline. It is read-only evidence from the
// same market database consumed by Strategy.
type DailyRankingRow struct {
	Date        uint64  `json:"date"`
	Rank        int     `json:"rank"`
	Pair        string  `json:"pair"`
	QuoteVolume float64 `json:"quote_volume"`
}

// BehaviourWindow contains enough canonical history to recompute a rolling
// strategy-input baseline without depending on dashboard-owned state.
type BehaviourWindow struct {
	LatestDate uint64            `json:"latestDate"`
	StartDate  uint64            `json:"startDate"`
	WarmupDate uint64            `json:"warmupDate"`
	Ranking    []DailyRankingRow `json:"ranking"`
	Bars       []Bar             `json:"bars"`
}

// LoadBehaviourWindow reads daily ranking rows plus bounded OHLCV warmup. The
// returned data is deliberately raw: provider code recomputes SMA Volume(25),
// RSI(7), top-N membership and daily baseline observations with the same helper
// formulas as the Market Data page.
func (r *Reader) LoadBehaviourWindow(ctx context.Context, canonicalTopN, historyDays, warmupDays int) (BehaviourWindow, error) {
	var result BehaviourWindow
	if r.databasePath == "" {
		return result, errors.New("market-data SQLite path is not configured")
	}
	if canonicalTopN <= 0 || canonicalTopN > 500 {
		return result, errors.New("canonical top-N must be between 1 and 500")
	}
	if historyDays < 10 || historyDays > 500 {
		return result, errors.New("behaviour history days must be between 10 and 500")
	}
	if warmupDays < 0 || warmupDays > 100 {
		return result, errors.New("behaviour warmup days must be between 0 and 100")
	}

	var latestRows []struct {
		LatestDate uint64 `json:"latest_date"`
	}
	if err := r.queryRows(ctx, `SELECT COALESCE(MAX(date), 0) AS latest_date FROM market_volume_rank_daily;`, &latestRows); err != nil {
		return result, err
	}
	if len(latestRows) != 1 || latestRows[0].LatestDate == 0 {
		return result, errors.New("canonical market-data database has no persisted ranking date")
	}
	result.LatestDate = latestRows[0].LatestDate

	startDate, err := subtractCalendarDays(result.LatestDate, historyDays-1)
	if err != nil {
		return result, err
	}
	warmupDate, err := subtractCalendarDays(startDate, warmupDays)
	if err != nil {
		return result, err
	}
	result.StartDate = startDate
	result.WarmupDate = warmupDate

	rankingSQL := fmt.Sprintf(
		`SELECT date, rank, pair, quote_volume FROM market_volume_rank_daily WHERE date BETWEEN %d AND %d AND rank <= %d ORDER BY date ASC, rank ASC;`,
		startDate, result.LatestDate, canonicalTopN,
	)
	if err := r.queryRows(ctx, rankingSQL, &result.Ranking); err != nil {
		return result, err
	}
	if len(result.Ranking) == 0 {
		return result, errors.New("canonical market-data database has no ranking rows in behaviour window")
	}

	symbolSet := make(map[string]struct{})
	for _, row := range result.Ranking {
		pair := strings.TrimSpace(row.Pair)
		if pair != "" {
			symbolSet[pair] = struct{}{}
		}
	}
	symbols := make([]string, 0, len(symbolSet))
	for pair := range symbolSet {
		symbols = append(symbols, pair)
	}
	if len(symbols) == 0 {
		return result, errors.New("behaviour ranking contains no symbols")
	}

	barsSQL := fmt.Sprintf(
		`SELECT pair, date, open, high, low, close, volume FROM ohlcv_data WHERE date BETWEEN %d AND %d AND pair IN (%s) ORDER BY pair ASC, date ASC;`,
		warmupDate, result.LatestDate, quotedStringList(symbols),
	)
	if err := r.queryRows(ctx, barsSQL, &result.Bars); err != nil {
		return result, err
	}
	return result, nil
}
