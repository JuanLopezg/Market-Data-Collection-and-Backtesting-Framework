package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"control-dashboard-api/internal/provider"
)

func main() {
	dsn := os.Getenv("DASHBOARD_POSTGRES_DSN")
	if dsn == "" {
		fail("DASHBOARD_POSTGRES_DSN is required")
	}

	p := provider.NewReal(provider.RealConfig{PostgreSQLDSN: dsn})

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	health := p.Health(ctx)
	if health.Mode != "real" {
		fail("unexpected provider mode: %s", health.Mode)
	}
	if health.ResourceCount != len(provider.AllResources) {
		fail("unexpected resource count: %d (want %d)", health.ResourceCount, len(provider.AllResources))
	}

	positions, err := p.Read(ctx, provider.ResourcePositions)
	if err != nil || !json.Valid(positions) {
		fail("positions read failed: %v", err)
	}
	execution, err := p.Read(ctx, provider.ResourceExecution)
	if err != nil || !json.Valid(execution) {
		fail("execution read failed: %v", err)
	}

	var positionsSummary struct {
		ActivePositions int `json:"activePositions"`
		Positions       []struct {
			Asset    string  `json:"asset"`
			LocalQty float64 `json:"localQty"`
		} `json:"positions"`
	}
	if err := json.Unmarshal(positions, &positionsSummary); err != nil {
		fail("decode positions response: %v", err)
	}

	var executionSummary struct {
		OpenOrders   int    `json:"openOrders"`
		FilledOrders int    `json:"filledOrders"`
		FillCount    uint64 `json:"fillCount"`
		Orders       []any  `json:"orders"`
	}
	if err := json.Unmarshal(execution, &executionSummary); err != nil {
		fail("decode execution response: %v", err)
	}

	fmt.Println("============================================================")
	fmt.Println("CONTROL DASHBOARD CURRENT POSTGRES READ-ONLY SMOKE")
	fmt.Println("============================================================")
	fmt.Printf("provider=%s ready=%t resources=%d\n", health.Name, health.Ready, health.ResourceCount)
	fmt.Printf("positions=%d\n", positionsSummary.ActivePositions)
	fmt.Printf("tracked_orders=%d open_orders=%d filled_orders=%d fills=%d\n", len(executionSummary.Orders), executionSummary.OpenOrders, executionSummary.FilledOrders, executionSummary.FillCount)
	fmt.Println("read_only_session=PASS (verified by provider health gate)")
	fmt.Println("POSTGRES READ-ONLY SMOKE: PASS")
	fmt.Println("============================================================")
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "FAIL: "+format+"\n", args...)
	os.Exit(1)
}
