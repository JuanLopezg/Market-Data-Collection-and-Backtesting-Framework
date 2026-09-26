package provider

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

const maxLedgerRecentEntries = 250

type LedgerEntry struct {
	EntryID       string  `json:"entryId"`
	FillID        string  `json:"fillId"`
	OrderID       string  `json:"orderId"`
	StrategyID    uint64  `json:"strategyId"`
	Timestamp     string  `json:"timestamp"`
	Asset         string  `json:"asset"`
	Side          string  `json:"side"`
	Quantity      float64 `json:"quantity"`
	Price         float64 `json:"price"`
	GrossNotional float64 `json:"grossNotional"`
	Commission    float64 `json:"commission"`
	PositionDelta float64 `json:"positionDelta"`
	CashDelta     float64 `json:"cashDelta"`
	EntryHash     string  `json:"entryHash"`
}

type LedgerStatus struct {
	Status                    string        `json:"status"`
	Validated                 bool          `json:"validated"`
	FoundationReady           bool          `json:"foundationReady"`
	SourceTable               string        `json:"sourceTable"`
	SourceContract            string        `json:"sourceContract"`
	AppendOnlySource          bool          `json:"appendOnlySource"`
	DeterministicProjection   bool          `json:"deterministicProjection"`
	TotalFillRows             int64         `json:"totalFillRows"`
	DistinctFillIDs           int64         `json:"distinctFillIds"`
	InvalidFillRows           int64         `json:"invalidFillRows"`
	LatestFillID              uint64        `json:"latestFillId"`
	LatestFillTimestamp       uint64        `json:"latestFillTimestamp"`
	TotalFees                 float64       `json:"totalFees"`
	TotalFeesLabel            string        `json:"totalFeesLabel"`
	GrossBuyNotional          float64       `json:"grossBuyNotional"`
	GrossSellNotional         float64       `json:"grossSellNotional"`
	NetCashDeltaFromFills     float64       `json:"netCashDeltaFromFills"`
	RecentWindowCount         int           `json:"recentWindowCount"`
	RecentWindowTruncated     bool          `json:"recentWindowTruncated"`
	RecentWindowFingerprint   string        `json:"recentWindowFingerprint"`
	Entries                   []LedgerEntry `json:"entries"`
	DurableRealizedPnL        bool          `json:"durableRealizedPnl"`
	DurableUnrealizedPnL      bool          `json:"durableUnrealizedPnl"`
	HistoricalEquityAvailable bool          `json:"historicalEquityAvailable"`
	ReadOnly                  bool          `json:"readOnly"`
	PrivateAuth               string        `json:"privateAuth"`
	OrderRouting              string        `json:"orderRouting"`
	CheckedAt                 string        `json:"checkedAt"`
	Error                     string        `json:"error,omitempty"`
	Note                      string        `json:"note"`
}

func buildLedgerStatus(window pgstore.LedgerFillWindow, checkedAt time.Time) LedgerStatus {
	result := LedgerStatus{
		Status:                    "BLOCKED",
		SourceTable:               "trading_fills",
		SourceContract:            "ExecutionState append-only fill history keyed by fill_id",
		AppendOnlySource:          true,
		DeterministicProjection:   true,
		TotalFillRows:             window.TotalRows,
		DistinctFillIDs:           window.DistinctFillIDs,
		InvalidFillRows:           window.InvalidRows,
		LatestFillID:              window.LatestFillID,
		LatestFillTimestamp:       window.LatestFillTimestamp,
		TotalFees:                 window.TotalCommission,
		TotalFeesLabel:            formatUSD(window.TotalCommission),
		GrossBuyNotional:          window.GrossBuyNotional,
		GrossSellNotional:         window.GrossSellNotional,
		NetCashDeltaFromFills:     window.GrossSellNotional - window.GrossBuyNotional - window.TotalCommission,
		DurableRealizedPnL:        false,
		DurableUnrealizedPnL:      false,
		HistoricalEquityAvailable: false,
		ReadOnly:                  true,
		PrivateAuth:               "DEFERRED",
		OrderRouting:              "DISABLED",
		CheckedAt:                 checkedAt.UTC().Format(time.RFC3339),
		Entries:                   []LedgerEntry{},
		Note:                      "Step 42 exposes a deterministic read-only economic ledger foundation over the runtime-owned append-only trading_fills table. Each persisted fill becomes one immutable economic event with explicit asset and cash deltas. This does NOT yet claim durable cost basis, realized/unrealized PnL attribution or historical equity; those require a runtime-owned accounting ledger/projection rather than dashboard reconstruction.",
	}

	if window.TotalRows < 0 || window.DistinctFillIDs < 0 || window.InvalidRows < 0 {
		result.Error = "ledger aggregate contains invalid negative counters"
		return result
	}
	if window.TotalRows != window.DistinctFillIDs {
		result.Error = fmt.Sprintf("fill_id uniqueness violation: rows=%d distinct=%d", window.TotalRows, window.DistinctFillIDs)
		return result
	}
	if window.InvalidRows != 0 {
		result.Error = fmt.Sprintf("trading_fills contains %d invalid economic row(s)", window.InvalidRows)
		return result
	}
	if !finiteNonNegative(window.TotalCommission) || !finiteNonNegative(window.GrossBuyNotional) || !finiteNonNegative(window.GrossSellNotional) {
		result.Error = "ledger aggregate contains non-finite or negative economics"
		return result
	}

	previous := strings.Repeat("0", 64)
	entries := make([]LedgerEntry, 0, len(window.Rows))
	for _, fill := range window.Rows {
		entry, err := ledgerEntryFromFill(fill, previous)
		if err != nil {
			result.Error = err.Error()
			return result
		}
		previous = entry.EntryHash
		entries = append(entries, entry)
	}
	result.RecentWindowFingerprint = previous
	if len(entries) == 0 {
		result.RecentWindowFingerprint = strings.Repeat("0", 64)
	}
	// SQL returns ascending order for deterministic fingerprinting. Present newest first.
	for i, j := 0, len(entries)-1; i < j; i, j = i+1, j-1 {
		entries[i], entries[j] = entries[j], entries[i]
	}
	result.Entries = entries
	result.RecentWindowCount = len(entries)
	result.RecentWindowTruncated = int64(len(entries)) < window.TotalRows
	result.Validated = true
	result.FoundationReady = true
	result.Status = "VALIDATED"
	return result
}

func ledgerEntryFromFill(fill tradingwire.Fill, previousHash string) (LedgerEntry, error) {
	asset := strings.TrimSpace(fill.Coin)
	if fill.FillID == 0 || fill.OrderID == 0 || fill.StrategyID == 0 || fill.Timestamp == 0 || asset == "" {
		return LedgerEntry{}, fmt.Errorf("fill %d is missing an immutable economic identity field", fill.FillID)
	}
	if fill.Side != 0 && fill.Side != 1 {
		return LedgerEntry{}, fmt.Errorf("fill %d has unsupported side=%d", fill.FillID, fill.Side)
	}
	if !finitePositive(fill.Quantity) || !finitePositive(fill.Price) || !finiteNonNegative(fill.Commission) {
		return LedgerEntry{}, fmt.Errorf("fill %d has invalid quantity/price/commission", fill.FillID)
	}

	side := "BUY"
	positionDelta := fill.Quantity
	gross := fill.Quantity * fill.Price
	cashDelta := -gross - fill.Commission
	if fill.Side == 1 {
		side = "SELL"
		positionDelta = -fill.Quantity
		cashDelta = gross - fill.Commission
	}
	if !isFiniteLedger(gross) || !isFiniteLedger(cashDelta) || !isFiniteLedger(positionDelta) {
		return LedgerEntry{}, fmt.Errorf("fill %d produces non-finite ledger economics", fill.FillID)
	}

	canonical := strings.Join([]string{
		previousHash,
		strconv.FormatUint(fill.FillID, 10),
		strconv.FormatUint(fill.OrderID, 10),
		strconv.FormatUint(fill.StrategyID, 10),
		strconv.FormatUint(fill.Timestamp, 10),
		asset,
		strconv.Itoa(fill.Side),
		strconv.FormatFloat(fill.Quantity, 'g', 17, 64),
		strconv.FormatFloat(fill.Price, 'g', 17, 64),
		strconv.FormatFloat(fill.Commission, 'g', 17, 64),
	}, "|")
	digest := sha256.Sum256([]byte(canonical))

	return LedgerEntry{
		EntryID:       "fill:" + strconv.FormatUint(fill.FillID, 10),
		FillID:        strconv.FormatUint(fill.FillID, 10),
		OrderID:       strconv.FormatUint(fill.OrderID, 10),
		StrategyID:    fill.StrategyID,
		Timestamp:     formatTradingTimestamp(fill.Timestamp),
		Asset:         asset,
		Side:          side,
		Quantity:      fill.Quantity,
		Price:         fill.Price,
		GrossNotional: gross,
		Commission:    fill.Commission,
		PositionDelta: positionDelta,
		CashDelta:     cashDelta,
		EntryHash:     hex.EncodeToString(digest[:]),
	}, nil
}

func finitePositive(v float64) bool    { return v > 0 && isFiniteLedger(v) }
func finiteNonNegative(v float64) bool { return v >= 0 && isFiniteLedger(v) }
func isFiniteLedger(v float64) bool    { return !math.IsNaN(v) && !math.IsInf(v, 0) }

func (p *Real) LedgerStatus(ctx context.Context) LedgerStatus {
	window, err := p.postgres.LedgerFills(ctx, maxLedgerRecentEntries)
	if err != nil {
		return LedgerStatus{
			Status: "BLOCKED", SourceTable: "trading_fills", SourceContract: "ExecutionState append-only fill history keyed by fill_id",
			AppendOnlySource: true, DeterministicProjection: true, ReadOnly: true, PrivateAuth: "DEFERRED", OrderRouting: "DISABLED",
			CheckedAt: time.Now().UTC().Format(time.RFC3339), Error: err.Error(),
			Note: "Step 42 cannot validate the append-only fill-ledger foundation because the canonical PostgreSQL source is unavailable. Order routing remains disabled.",
		}
	}
	return buildLedgerStatus(window, time.Now().UTC())
}

func (p *EmbeddedMock) LedgerStatus(_ context.Context) LedgerStatus {
	return LedgerStatus{
		Status: "MOCK_ONLY", Validated: false, FoundationReady: false, SourceTable: "mock", SourceContract: "mock provider",
		AppendOnlySource: false, DeterministicProjection: false, ReadOnly: true, PrivateAuth: "DEFERRED", OrderRouting: "DISABLED", CheckedAt: "mock",
		Entries: []LedgerEntry{}, Note: "Mock mode cannot validate the real Step 42 append-only ledger foundation.",
	}
}
