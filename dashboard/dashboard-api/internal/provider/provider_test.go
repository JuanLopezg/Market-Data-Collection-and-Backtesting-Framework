package provider

import (
	"context"
	"encoding/json"
	"testing"
)

func TestEmbeddedMockCoversAllResources(t *testing.T) {
	p, err := NewEmbeddedMock()
	if err != nil {
		t.Fatalf("NewEmbeddedMock() error = %v", err)
	}
	health := p.Health(context.Background())
	if !health.Ready || health.ResourceCount != len(AllResources) {
		t.Fatalf("unexpected health: %+v", health)
	}
	for _, resource := range AllResources {
		body, err := p.Read(context.Background(), resource)
		if err != nil {
			t.Fatalf("Read(%s) error = %v", resource, err)
		}
		if !json.Valid(body) {
			t.Fatalf("Read(%s) returned invalid JSON", resource)
		}
	}
}

func TestRealProviderAdvertisesAllRealResourcesButProviderReadinessStaysFailClosed(t *testing.T) {
	p := NewReal(RealConfig{PostgreSQLDSN: "host=postgres port=5432", NATSURL: "nats://nats:4222", MarketDataDB: "/data/market/database.db"})
	health := p.Health(context.Background())
	if health.Ready {
		t.Fatal("real provider must not report global trading readiness")
	}
	if health.ResourceCount != len(AllResources) {
		t.Fatalf("real provider resource count = %d, want %d", health.ResourceCount, len(AllResources))
	}
}
