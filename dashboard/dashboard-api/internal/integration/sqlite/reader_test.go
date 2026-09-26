package sqlite

import (
	"context"
	"strings"
	"testing"
	"time"
)

type fakeRunner struct {
	responses [][]byte
	queries   []string
}

func (f *fakeRunner) QueryJSON(_ context.Context, _ string, query string) ([]byte, error) {
	f.queries = append(f.queries, query)
	if len(f.responses) == 0 {
		return []byte(`[]`), nil
	}
	value := f.responses[0]
	f.responses = f.responses[1:]
	return value, nil
}

func TestLoadStrategyWindowUsesBoundedReadOnlyQueries(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`[{"latest_date":20260919}]`),
		[]byte(`[{"rank":1,"pair":"BTCUSDT","quote_volume":1000},{"rank":2,"pair":"ETHUSDT","quote_volume":900}]`),
		[]byte(`[{"pair":"BTCUSDT","date":20260919,"open":1,"high":2,"low":1,"close":2,"volume":10},{"pair":"ETHUSDT","date":20260919,"open":2,"high":3,"low":2,"close":3,"volume":20}]`),
		[]byte(`[{"duplicateRows":0,"invalidRows":0}]`),
	}}
	reader := NewReader(ReaderConfig{DatabasePath: "/data/market/database.db", Timeout: time.Second, Runner: runner})
	window, err := reader.LoadStrategyWindow(context.Background(), 50, 100)
	if err != nil {
		t.Fatal(err)
	}
	if window.LatestDate != 20260919 || window.StartDate != 20260612 || len(window.Ranking) != 2 || len(window.Bars) != 2 {
		t.Fatalf("unexpected window: %+v", window)
	}
	if len(runner.queries) != 4 || !strings.Contains(runner.queries[1], "rank <= 50") || !strings.Contains(runner.queries[2], "date BETWEEN 20260612 AND 20260919") {
		t.Fatalf("unexpected bounded queries: %#v", runner.queries)
	}
}

func TestQuotedStringListEscapesValues(t *testing.T) {
	value := quotedStringList([]string{"ETHUSDT", "X'Y"})
	if value != "'ETHUSDT','X''Y'" {
		t.Fatalf("unexpected SQL list: %s", value)
	}
}

func TestLoadBehaviourWindowReadsDailyRankingAndWarmupBars(t *testing.T) {
	runner := &fakeRunner{responses: [][]byte{
		[]byte(`[{"latest_date":20260919}]`),
		[]byte(`[{"date":20260918,"rank":1,"pair":"BTCUSDT","quote_volume":1000},{"date":20260919,"rank":1,"pair":"BTCUSDT","quote_volume":1100}]`),
		[]byte(`[{"pair":"BTCUSDT","date":20260601,"open":1,"high":2,"low":1,"close":2,"volume":10},{"pair":"BTCUSDT","date":20260919,"open":2,"high":3,"low":2,"close":3,"volume":20}]`),
	}}
	reader := NewReader(ReaderConfig{DatabasePath: "/data/market/database.db", Timeout: time.Second, Runner: runner})
	window, err := reader.LoadBehaviourWindow(context.Background(), 50, 60, 35)
	if err != nil {
		t.Fatal(err)
	}
	if window.LatestDate != 20260919 || window.StartDate != 20260722 || window.WarmupDate != 20260617 {
		t.Fatalf("unexpected dates: %+v", window)
	}
	if len(window.Ranking) != 2 || len(window.Bars) != 2 {
		t.Fatalf("unexpected behaviour window: %+v", window)
	}
	if len(runner.queries) != 3 || !strings.Contains(runner.queries[1], "date BETWEEN 20260722 AND 20260919") || !strings.Contains(runner.queries[2], "date BETWEEN 20260617 AND 20260919") {
		t.Fatalf("unexpected behaviour queries: %#v", runner.queries)
	}
}

func TestSQLiteCLIArgsUseSilentBusyTimeout(t *testing.T) {
	args := sqliteCLIArgs("/data/market/database.db", "SELECT 1;")
	joined := strings.Join(args, "\x00")
	if !strings.Contains(joined, ".timeout 1000") {
		t.Fatalf("sqlite CLI args must configure a bounded busy wait via .timeout: %#v", args)
	}
	if strings.Contains(joined, "PRAGMA busy_timeout") {
		t.Fatalf("PRAGMA busy_timeout must not be used in CLI args because it emits a value that pollutes JSON stdout: %#v", args)
	}
	if len(args) < 2 || args[len(args)-2] != "/data/market/database.db" || args[len(args)-1] != "SELECT 1;" {
		t.Fatalf("database/query must remain the final sqlite CLI arguments: %#v", args)
	}
}
