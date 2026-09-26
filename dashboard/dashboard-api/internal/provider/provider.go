package provider

import (
	"context"
	"encoding/json"
	"errors"
)

type Resource string

const (
	ResourceShellStatus    Resource = "shell-status"
	ResourceOverview       Resource = "overview"
	ResourcePositions      Resource = "positions"
	ResourceReconciliation Resource = "reconciliation"
	ResourcePipeline       Resource = "pipeline"
	ResourceExecution      Resource = "execution"
	ResourceRisk           Resource = "risk"
	ResourceMarketData     Resource = "market-data"
	ResourceInfrastructure Resource = "infrastructure"
	ResourceAlertsAudit    Resource = "alerts-audit"
	ResourceLiveVsExpected Resource = "live-vs-expected"
	ResourceManualControl  Resource = "manual-control"
)

var AllResources = []Resource{
	ResourceShellStatus,
	ResourceOverview,
	ResourcePositions,
	ResourceReconciliation,
	ResourcePipeline,
	ResourceExecution,
	ResourceRisk,
	ResourceMarketData,
	ResourceInfrastructure,
	ResourceAlertsAudit,
	ResourceLiveVsExpected,
	ResourceManualControl,
}

type Health struct {
	Name          string `json:"name"`
	Mode          string `json:"mode"`
	Ready         bool   `json:"ready"`
	Detail        string `json:"detail"`
	ResourceCount int    `json:"resourceCount"`
}

type Provider interface {
	Read(ctx context.Context, resource Resource) (json.RawMessage, error)
	Health(ctx context.Context) Health
}

var (
	ErrResourceUnavailable       = errors.New("dashboard resource unavailable")
	ErrMappingUnverified         = errors.New("real data mapping has not been verified against the trading codebase")
	ErrIntegrationNotImplemented = errors.New("real data mapping is verified but the integration adapter is not implemented")
)
