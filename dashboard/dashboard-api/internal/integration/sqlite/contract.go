// Package sqlite defines the read-only boundary for canonical market data.
// The uploaded runtime owns and commits this database before MARKET_DATA_UPDATED
// is published; the dashboard may only open it read-only.
package sqlite

import "context"

type Health struct {
	Ready  bool   `json:"ready"`
	Detail string `json:"detail"`
}

type MarketDataStore interface {
	Health(ctx context.Context) (Health, error)
}
