package provider

import (
	"context"
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"strings"
)

//go:embed fixtures/*.json
var embeddedFixtures embed.FS

type EmbeddedMock struct {
	fixtures map[Resource]json.RawMessage
}

func NewEmbeddedMock() (*EmbeddedMock, error) {
	entries, err := fs.Glob(embeddedFixtures, "fixtures/*.json")
	if err != nil {
		return nil, fmt.Errorf("glob provider fixtures: %w", err)
	}

	fixtures := make(map[Resource]json.RawMessage, len(entries))
	for _, entry := range entries {
		body, err := embeddedFixtures.ReadFile(entry)
		if err != nil {
			return nil, fmt.Errorf("read provider fixture %s: %w", entry, err)
		}
		if !json.Valid(body) {
			return nil, fmt.Errorf("provider fixture %s is not valid JSON", entry)
		}
		name := Resource(strings.TrimSuffix(strings.TrimPrefix(entry, "fixtures/"), ".json"))
		fixtures[name] = append(json.RawMessage(nil), body...)
	}

	if len(fixtures) != len(AllResources) {
		return nil, fmt.Errorf("provider fixture count = %d, want %d", len(fixtures), len(AllResources))
	}
	for _, resource := range AllResources {
		if _, ok := fixtures[resource]; !ok {
			return nil, fmt.Errorf("missing provider fixture for %s", resource)
		}
	}

	return &EmbeddedMock{fixtures: fixtures}, nil
}

func (p *EmbeddedMock) Read(_ context.Context, resource Resource) (json.RawMessage, error) {
	body, ok := p.fixtures[resource]
	if !ok {
		return nil, fmt.Errorf("%w: %s", ErrResourceUnavailable, resource)
	}
	return append(json.RawMessage(nil), body...), nil
}

func (p *EmbeddedMock) Health(_ context.Context) Health {
	return Health{
		Name:          "embedded-mock-fixtures",
		Mode:          "mock",
		Ready:         true,
		Detail:        "server-side fixtures; no trading-system connectivity",
		ResourceCount: len(p.fixtures),
	}
}

func (p *EmbeddedMock) PreviewManualControl(_ context.Context, req ManualControlPreviewRequest) (ManualControlData, error) {
	body, ok := p.fixtures[ResourceManualControl]
	if !ok {
		return ManualControlData{}, fmt.Errorf("%w: %s", ErrResourceUnavailable, ResourceManualControl)
	}
	var value ManualControlData
	if err := json.Unmarshal(body, &value); err != nil {
		return ManualControlData{}, fmt.Errorf("decode mock manual-control fixture: %w", err)
	}
	value.ContractVersion = ManualControlContractVersion
	value.RoutingContractReady = false
	value.RoutingContractMode = "ADMISSION_ONLY_FAIL_CLOSED"
	value.ExecutionBoundary = "OPERATOR -> dashboard admission -> future trading-control service -> PortfolioRisk -> OrderPlanner -> ExecutionState -> VenueAdapter"
	value.ExchangeConstraintsValidated = false
	value.TradingControlSink = "UNCONFIGURED"
	value.PrivateAuth = "DEFERRED"
	value.OrderLifecycle = "DEFERRED"
	value.ConfirmationRequired = true
	value.ConfirmationPhrase = ManualControlConfirmationPhrase
	value.HumanAuditAvailable = false
	value.RouteBlockers = []string{"MOCK_PROVIDER", "MANUAL_RISK_CONTRACT_UNAVAILABLE", "TRADING_CONTROL_SINK_UNCONFIGURED", "PRIVATE_AUTH_DEFERRED", "ORDER_LIFECYCLE_DEFERRED"}
	value.RecentRouteAudits = []ManualRouteAuditRow{}
	value.AuditActorLabel = strings.TrimSpace(req.Actor)
	value.BackendAuthoritative = false
	value.ValidationPassed = true
	value.RiskCheckAvailable = false
	value.RouteEnabled = false
	value.SourceMode = "MOCK"
	value.PreviewKind = "MOCK_TARGET_DELTA"
	value.SafetyNote = "Mock preview only; Step 46 routing admission remains fail-closed and no trading action occurs."
	return value, nil
}
