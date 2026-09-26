// Package nats defines the future boundary for event-derived snapshots. It does
// not depend on a NATS client yet because subjects/codecs must be verified first.
package nats

import (
	"context"
	"encoding/json"
)

type Health struct {
	Ready  bool
	Detail string
}

// SnapshotSource represents a dashboard-owned projection of messaging state.
// The browser never subscribes to NATS directly.
type SnapshotSource interface {
	SnapshotJSON(ctx context.Context, projection string) (json.RawMessage, error)
	Health(ctx context.Context) (Health, error)
}
