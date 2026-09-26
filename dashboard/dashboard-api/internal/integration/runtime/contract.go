// Package runtime defines a narrow boundary for live service state that is not
// naturally sourced from durable read models or event projections.
package runtime

import (
	"context"
	"encoding/json"
)

type Health struct {
	Ready  bool
	Detail string
}

// ServiceStateSource is deliberately transport-neutral. The final adapter may
// use an existing internal API, a dedicated observability endpoint or another
// project-native mechanism after the codebase is reviewed.
type ServiceStateSource interface {
	ReadJSON(ctx context.Context, capability string) (json.RawMessage, error)
	Health(ctx context.Context) (Health, error)
}
