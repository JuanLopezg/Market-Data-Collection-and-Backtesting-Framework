package postgres

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// Accounting projection written by ExecutionState (C++ PositionLedger): open and closed
// positions with cost basis, realized PnL, fees and funding, plus the account equity
// marked at each daily close. Available is false while the running ExecutionState is an
// older version that does not write these tables yet.
type Accounting struct {
	Available bool
	Open      []AccountingPosition `json:"open"`
	Closed    []AccountingPosition `json:"closed"`
	Equity    []EquityDay          `json:"equity"`
}

type AccountingPosition struct {
	Coin          string  `json:"coin"`
	Side          string  `json:"side"`
	OpenedAt      uint64  `json:"opened_at"`
	ClosedAt      uint64  `json:"closed_at"`
	ClosingFillID uint64  `json:"closing_fill_id"`
	Quantity      float64 `json:"quantity"`
	MaxQuantity   float64 `json:"max_quantity"`
	EntryPrice    float64 `json:"entry_price"`
	EntryValue    float64 `json:"entry_value"`
	ExitValue     float64 `json:"exit_value"`
	ExitQuantity  float64 `json:"exit_quantity"`
	ExitPrice     float64 `json:"exit_price"`
	GrossPnL      float64 `json:"gross_pnl"`
	Fees          float64 `json:"fees"`
	Funding       float64 `json:"funding"`
}

type EquityDay struct {
	Date             uint64  `json:"date"`
	Cash             float64 `json:"cash"`
	PositionsValue   float64 `json:"positions_value"`
	Equity           float64 `json:"equity"`
	RealizedGrossPnL float64 `json:"realized_gross_pnl"`
	Fees             float64 `json:"fees"`
	Funding          float64 `json:"funding"`
	OpenPositions    int     `json:"open_positions"`
	// Real venues only (0 in PAPER and before 2026-10-14): totals up to that close and the
	// venue collateral (BTC after haircut) sized with that day. Equity excludes collateral.
	CapitalFlows  float64 `json:"capital_flows"`
	Interest      float64 `json:"interest"`
	CollateralUSD float64 `json:"collateral_usd"`
}

const accountingPresentSQL = `SELECT json_build_object('present', to_regclass('trading_equity_daily') IS NOT NULL AND to_regclass('trading_closed_positions') IS NOT NULL AND to_regclass('trading_open_positions') IS NOT NULL);`

const accountingSQL = `SELECT json_build_object(
  'open', (SELECT coalesce(json_agg(row_to_json(o) ORDER BY o.coin), '[]'::json) FROM trading_open_positions o),
  'closed', (SELECT coalesce(json_agg(row_to_json(c) ORDER BY c.closed_at, c.closing_fill_id), '[]'::json) FROM trading_closed_positions c),
  'equity', (SELECT coalesce(json_agg(row_to_json(e) ORDER BY e.date), '[]'::json) FROM (
      SELECT * FROM trading_equity_daily) e)
);`

// "SELECT *" so a database without the 2026-10-14 columns (older execution-state) still
// reads; the missing fields decode as 0.

// Accounting reads the runtime-owned position and equity tables.
func (r *Reader) Accounting(ctx context.Context) (Accounting, error) {
	var result Accounting
	if r.dsn == "" {
		return result, errors.New("PostgreSQL DSN is not configured")
	}
	raw, err := r.query(ctx, accountingPresentSQL)
	if err != nil {
		return result, err
	}
	var present struct {
		Present bool `json:"present"`
	}
	if err := json.Unmarshal([]byte(strings.TrimSpace(string(raw))), &present); err != nil {
		return result, fmt.Errorf("decode accounting table check: %w", err)
	}
	if !present.Present {
		return result, nil
	}
	raw, err = r.query(ctx, accountingSQL)
	if err != nil {
		return result, err
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return result, fmt.Errorf("decode accounting projection: %w", err)
	}
	result.Available = true
	return result, nil
}
