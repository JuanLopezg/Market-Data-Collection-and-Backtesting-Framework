package provider

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestMarketDataResourceReusesFreshSuccessfulSnapshot(t *testing.T) {
	p := &Real{}
	p.marketCache.value = realMarketData{Source: "cached", TotalAssets: 20}
	p.marketCache.finished = time.Now()

	got, err := p.marketDataResource(context.Background())
	if err != nil {
		t.Fatalf("fresh cached market snapshot returned error: %v", err)
	}
	if got.Source != "cached" || got.TotalAssets != 20 {
		t.Fatalf("unexpected cached market snapshot: %+v", got)
	}
}

func TestMarketDataResourceSharesInflightResultWithWaiter(t *testing.T) {
	p := &Real{}
	done := make(chan struct{})
	p.marketCache.inFlight = true
	p.marketCache.done = done

	resultCh := make(chan realMarketData, 1)
	errCh := make(chan error, 1)
	go func() {
		got, err := p.marketDataResource(context.Background())
		resultCh <- got
		errCh <- err
	}()

	time.Sleep(10 * time.Millisecond)
	p.marketCache.mu.Lock()
	p.marketCache.value = realMarketData{Source: "shared", TotalAssets: 10}
	p.marketCache.err = errors.New("shared failure")
	p.marketCache.finished = time.Now()
	p.marketCache.inFlight = false
	close(done)
	p.marketCache.mu.Unlock()

	select {
	case got := <-resultCh:
		if got.Source != "shared" || got.TotalAssets != 10 {
			t.Fatalf("waiter did not receive inflight result: %+v", got)
		}
	case <-time.After(time.Second):
		t.Fatal("waiter did not complete")
	}
	if err := <-errCh; err == nil || err.Error() != "shared failure" {
		t.Fatalf("waiter did not receive inflight error: %v", err)
	}
}

func TestInfrastructureReusesFreshSnapshot(t *testing.T) {
	p := &Real{}
	p.infraCache.value = realInfrastructure{Readiness: "DEGRADED", SourceMode: "REAL"}
	p.infraCache.finished = time.Now()
	got := p.infrastructure(context.Background())
	if got.Readiness != "DEGRADED" || got.SourceMode != "REAL" {
		t.Fatalf("unexpected cached infrastructure snapshot: %+v", got)
	}
}
