package postgres

import (
	"context"
	"fmt"
	"testing"
)

func TestRuntimeStatePositionStopPriceOptional(t *testing.T) {
	for _, test := range []struct {
		name, field string
		want        float64
	}{
		{"old market order", "", 0},
		{"resting stop", `,"position_stop_price":3.53745`, 3.53745},
	} {
		t.Run(test.name, func(t *testing.T) {
			raw := fmt.Sprintf(`{"updatedAt":"2026-10-17T00:00:00Z","snapshot":{"schema_version":1,"account_cash":100,"account_positions":{"DOTUSDT":2},"orders":[{"order_id":7,"coin":"DOTUSDT","side":1,"quantity":2,"status":2%s}]}}`, test.field)
			runner := &fakeRunner{responses: [][]byte{[]byte(raw)}}
			reader := NewReader(Config{DSN: "host=fixture dbname=fixture", Runner: runner})
			state, err := reader.RuntimeState(context.Background())
			if err != nil {
				t.Fatal(err)
			}
			if len(state.Snapshot.Orders) != 1 || state.Snapshot.Orders[0].PositionStopPrice != test.want {
				t.Fatalf("unexpected stop decode: %+v", state.Snapshot.Orders)
			}
		})
	}
}
