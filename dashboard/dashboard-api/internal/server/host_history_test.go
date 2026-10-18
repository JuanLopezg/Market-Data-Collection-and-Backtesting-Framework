package server

import (
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestHostHistoryAllUsesEntireRetainedSpanAndBoundsPoints(t *testing.T) {
	now := time.Now().UTC().Truncate(time.Second)
	oldest := now.Add(-15 * 24 * time.Hour)
	path := filepath.Join(t.TempDir(), "history.jsonl")
	content := "not json\n"
	// Reverse order proves ALL finds the earliest observation, not the first line.
	for i := 1000; i >= 0; i-- {
		observed := oldest.Add(time.Duration(i) * (now.Sub(oldest) / 1000))
		content += fmt.Sprintf("{\"t\":%q,\"cpuPct\":10}\n", observed.Format(time.RFC3339Nano))
	}
	content += fmt.Sprintf("{\"t\":%q,\"cpuPct\":99}\n", now.Add(24*time.Hour).Format(time.RFC3339))
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	s := &Server{cfg: Config{HostHistoryFile: path}}
	response := httptest.NewRecorder()
	s.hostHistory(response, httptest.NewRequest("GET", "/api/infrastructure/history?range=all", nil))
	if response.Code != 200 {
		t.Fatalf("ALL rejected: %d %s", response.Code, response.Body.String())
	}
	var result hostHistory
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !result.Available || result.Range != "all" || result.OldestAt != oldest.Format(time.RFC3339) || result.Stale {
		t.Fatalf("wrong retained span: %+v", result)
	}
	if len(result.Points) > maxHistoryPoints || len(result.Points) < 2 {
		t.Fatalf("unexpected point count: %d", len(result.Points))
	}
	for _, point := range result.Points {
		if point.CPUPct == nil || *point.CPUPct != 10 {
			t.Fatalf("future data must be excluded: %+v", point)
		}
	}
}

func TestReadHostHistoryAveragesBucketsAndKeepsMissingValuesAbsent(t *testing.T) {
	now := time.Date(2026, 10, 11, 12, 0, 0, 0, time.UTC)
	lines := []string{
		// Outside the 1h window: ignored.
		`{"t":"2026-10-11T10:00:00Z","cpuPct":99,"ramUsedMiB":1,"ramTotalMiB":2,"diskUsedGiB":1,"diskTotalGiB":2,"containers":{}}`,
		`{"t":"2026-10-11T11:30:05Z","cpuPct":10,"ramUsedMiB":1000,"ramTotalMiB":2000,"diskUsedGiB":10,"diskTotalGiB":50,"containers":{"strategy":[2,100],"nats":[null,50]}}`,
		`{"t":"2026-10-11T11:30:20Z","cpuPct":20,"ramUsedMiB":1200,"ramTotalMiB":2000,"diskUsedGiB":10,"diskTotalGiB":50,"containers":{"strategy":[4,120]}}`,
		`not json: a partially written line is skipped`,
		`{"t":"2026-10-11T11:59:40Z","cpuPct":30,"ramUsedMiB":1100,"ramTotalMiB":2000,"diskUsedGiB":11,"diskTotalGiB":50,"containers":{}}`,
	}
	path := filepath.Join(t.TempDir(), "history.jsonl")
	content := ""
	for _, line := range lines {
		content += line + "\n"
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}

	result, err := readHostHistory(path, "1h", now.Add(-time.Hour), now, now)
	if err != nil {
		t.Fatal(err)
	}
	if !result.Available || result.BucketSeconds != 30 || len(result.Points) != 2 {
		t.Fatalf("unexpected history shape: %+v", result)
	}
	first := result.Points[0]
	if *first.CPUPct != 15 || *first.RAMUsedMiB != 1100 {
		t.Fatalf("bucket mean not applied: cpu %v ram %v", *first.CPUPct, *first.RAMUsedMiB)
	}
	if first.ContainerCPUPct["strategy"] != 3 || first.ContainerRAMMiB["strategy"] != 110 {
		t.Fatalf("container mean not applied: %+v", first)
	}
	if _, present := first.ContainerCPUPct["nats"]; present {
		t.Fatal("missing container CPU must stay absent, not zero")
	}
	if first.ContainerRAMMiB["nats"] != 50 {
		t.Fatal("available container RAM must be kept")
	}
	if len(result.Containers) != 2 || result.Containers[0] != "nats" || result.Stale {
		t.Fatalf("unexpected containers/staleness: %+v %v", result.Containers, result.Stale)
	}

	stale, err := readHostHistory(path, "1h", now.Add(-50*time.Minute), now.Add(10*time.Minute), now.Add(10*time.Minute))
	if err != nil || !stale.Stale {
		t.Fatalf("history older than two minutes must be flagged stale: %v %+v", err, stale)
	}

	// Explicit window: only rows inside [from, to].
	custom, err := readHostHistory(path, "custom", time.Date(2026, 10, 11, 11, 59, 0, 0, time.UTC), now, now)
	if err != nil || len(custom.Points) != 1 || *custom.Points[0].CPUPct != 30 {
		t.Fatalf("explicit window not applied: %v %+v", err, custom)
	}
}
