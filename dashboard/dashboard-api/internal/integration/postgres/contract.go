// Package postgres defines the boundary the dashboard may use for durable read
// models. It intentionally contains no SQL driver and no schema assumptions yet.
package postgres

import (
	"context"
	"encoding/json"
)

type Health struct {
	Ready   bool
	Latency string
	Detail  string
}

// ReadModelStore is the future adapter boundary for dashboard-specific durable
// projections. model and params are dashboard concepts, not raw browser SQL.
type ReadModelStore interface {
	QueryJSON(ctx context.Context, model string, params map[string]string) (json.RawMessage, error)
	Health(ctx context.Context) (Health, error)
}
