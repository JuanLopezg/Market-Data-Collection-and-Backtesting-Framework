package server

import (
	"bufio"
	"encoding/json"
	"errors"
	"math"
	"net/http"
	"os"
	"sort"
	"time"
)

// Host resource history for the Infrastructure CPU/RAM/disk charts.
//
// deploy/chain/monitor.py appends one row to history.jsonl every 30 seconds and keeps
// seven days. This endpoint returns the requested window averaged into at most
// maxHistoryPoints buckets. Missing values stay absent; nothing is interpolated.

const (
	maxHistoryPoints    = 720
	maxHistoryFileBytes = 64 << 20
	historyFreshness    = 2 * time.Minute
	historyRetention    = 8 * 24 * time.Hour // Monitor keeps seven days; one day of margin.
)

var historyRanges = map[string]time.Duration{
	"1h":  time.Hour,
	"6h":  6 * time.Hour,
	"24h": 24 * time.Hour,
	"7d":  7 * 24 * time.Hour,
}

type historyRow struct {
	Time         string                 `json:"t"`
	CPUPct       *float64               `json:"cpuPct"`
	RAMUsedMiB   *float64               `json:"ramUsedMiB"`
	RAMTotalMiB  *float64               `json:"ramTotalMiB"`
	DiskUsedGiB  *float64               `json:"diskUsedGiB"`
	DiskTotalGiB *float64               `json:"diskTotalGiB"`
	Containers   map[string][2]*float64 `json:"containers"` // name -> [cpu % of host, RAM MiB]
}

type hostHistoryPoint struct {
	Time            string             `json:"t"`
	CPUPct          *float64           `json:"cpuPct"`
	RAMUsedMiB      *float64           `json:"ramUsedMiB"`
	RAMTotalMiB     *float64           `json:"ramTotalMiB"`
	DiskUsedGiB     *float64           `json:"diskUsedGiB"`
	DiskTotalGiB    *float64           `json:"diskTotalGiB"`
	ContainerCPUPct map[string]float64 `json:"containerCpuPct"`
	ContainerRAMMiB map[string]float64 `json:"containerRamMiB"`
}

type hostHistory struct {
	Available     bool               `json:"available"`
	Reason        string             `json:"reason,omitempty"`
	Range         string             `json:"range"`
	BucketSeconds int                `json:"bucketSeconds"`
	OldestAt      string             `json:"oldestAt,omitempty"`
	NewestAt      string             `json:"newestAt,omitempty"`
	Stale         bool               `json:"stale"`
	Containers    []string           `json:"containers"`
	Points        []hostHistoryPoint `json:"points"`
}

// Running mean of one optional value.
type mean struct {
	sum   float64
	count int
}

func (m *mean) add(value *float64) {
	if value != nil && !math.IsNaN(*value) && !math.IsInf(*value, 0) {
		m.sum += *value
		m.count++
	}
}

func (m mean) value() *float64 {
	if m.count == 0 {
		return nil
	}
	result := m.sum / float64(m.count)
	return &result
}

type historyBucket struct {
	cpu, ramUsed, ramTotal, diskUsed, diskTotal mean
	containerCPU, containerRAM                  map[string]*mean
}

func readHostHistory(path, rangeName string, start, end, now time.Time) (hostHistory, error) {
	result := hostHistory{Range: rangeName, Containers: []string{}, Points: []hostHistoryPoint{}}
	info, err := os.Stat(path)
	if err != nil {
		return result, err
	}
	if info.Size() > maxHistoryFileBytes {
		return result, errors.New("host history file exceeds its size bound")
	}
	file, err := os.Open(path)
	if err != nil {
		return result, err
	}
	defer file.Close()

	if rangeName == "all" {
		// Find the actual retained span without loading the file into memory.
		// Rows need not be sorted; malformed and future observations are ignored.
		var oldest time.Time
		scan := bufio.NewScanner(file)
		scan.Buffer(make([]byte, 64*1024), 1<<20)
		for scan.Scan() {
			var row historyRow
			if json.Unmarshal(scan.Bytes(), &row) != nil {
				continue
			}
			observed, err := time.Parse(time.RFC3339Nano, row.Time)
			if err == nil && !observed.After(end) && (oldest.IsZero() || observed.Before(oldest)) {
				oldest = observed
			}
		}
		if err := scan.Err(); err != nil {
			return result, err
		}
		start = now
		if !oldest.IsZero() {
			start = oldest
			result.OldestAt = oldest.UTC().Format(time.RFC3339)
		}
		if _, err := file.Seek(0, 0); err != nil {
			return result, err
		}
	}

	bucketSize := end.Sub(start) / maxHistoryPoints
	if bucketSize < 30*time.Second {
		bucketSize = 30 * time.Second
	}
	buckets := map[int64]*historyBucket{}
	names := map[string]bool{}
	var newest time.Time

	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		var row historyRow
		if json.Unmarshal(scanner.Bytes(), &row) != nil {
			continue // A partially written last line is skipped, not fatal.
		}
		observed, err := time.Parse(time.RFC3339Nano, row.Time)
		if err != nil || observed.Before(start) || observed.After(end) {
			continue
		}
		if observed.After(newest) {
			newest = observed
		}
		index := int64(observed.Sub(start) / bucketSize)
		if index >= maxHistoryPoints {
			index = maxHistoryPoints - 1
		}
		bucket := buckets[index]
		if bucket == nil {
			bucket = &historyBucket{containerCPU: map[string]*mean{}, containerRAM: map[string]*mean{}}
			buckets[index] = bucket
		}
		bucket.cpu.add(row.CPUPct)
		bucket.ramUsed.add(row.RAMUsedMiB)
		bucket.ramTotal.add(row.RAMTotalMiB)
		bucket.diskUsed.add(row.DiskUsedGiB)
		bucket.diskTotal.add(row.DiskTotalGiB)
		for name, values := range row.Containers {
			names[name] = true
			if bucket.containerCPU[name] == nil {
				bucket.containerCPU[name] = &mean{}
				bucket.containerRAM[name] = &mean{}
			}
			bucket.containerCPU[name].add(values[0])
			bucket.containerRAM[name].add(values[1])
		}
	}
	if err := scanner.Err(); err != nil {
		return result, err
	}

	indexes := make([]int64, 0, len(buckets))
	for index := range buckets {
		indexes = append(indexes, index)
	}
	sort.Slice(indexes, func(a, b int) bool { return indexes[a] < indexes[b] })
	for _, index := range indexes {
		bucket := buckets[index]
		point := hostHistoryPoint{
			Time:            start.Add(time.Duration(index)*bucketSize + bucketSize/2).UTC().Format(time.RFC3339),
			CPUPct:          bucket.cpu.value(),
			RAMUsedMiB:      bucket.ramUsed.value(),
			RAMTotalMiB:     bucket.ramTotal.value(),
			DiskUsedGiB:     bucket.diskUsed.value(),
			DiskTotalGiB:    bucket.diskTotal.value(),
			ContainerCPUPct: map[string]float64{},
			ContainerRAMMiB: map[string]float64{},
		}
		for name := range bucket.containerCPU {
			if value := bucket.containerCPU[name].value(); value != nil {
				point.ContainerCPUPct[name] = *value
			}
			if value := bucket.containerRAM[name].value(); value != nil {
				point.ContainerRAMMiB[name] = *value
			}
		}
		result.Points = append(result.Points, point)
	}
	for name := range names {
		result.Containers = append(result.Containers, name)
	}
	sort.Strings(result.Containers)

	result.BucketSeconds = int(bucketSize / time.Second)
	result.Available = len(result.Points) > 0
	if !result.Available {
		result.Reason = "No host observations in this range"
	}
	if !newest.IsZero() {
		result.NewestAt = newest.UTC().Format(time.RFC3339)
		result.Stale = now.Sub(newest) > historyFreshness
	}
	return result, nil
}

// GET /api/infrastructure/history?range=1h|6h|24h|7d|all (ending now)
// or ?from=RFC3339&to=RFC3339 (explicit UTC window within the retained history).
func (s *Server) hostHistory(w http.ResponseWriter, r *http.Request) {
	now := time.Now().UTC()
	query := r.URL.Query()
	rangeName := query.Get("range")
	var start, end time.Time
	if query.Get("from") != "" || query.Get("to") != "" {
		from, errFrom := time.Parse(time.RFC3339, query.Get("from"))
		to, errTo := time.Parse(time.RFC3339, query.Get("to"))
		if errFrom != nil || errTo != nil || !to.After(from.Add(time.Minute-1)) || from.Before(now.Add(-historyRetention)) || from.After(now) {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "from/to must be RFC 3339 UTC times, at least one minute apart, within the last seven days"})
			return
		}
		rangeName, start, end = "custom", from.UTC(), to.UTC()
	} else {
		if rangeName == "" {
			rangeName = "6h"
		}
		window, ok := historyRanges[rangeName]
		if !ok && rangeName != "all" {
			s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "range must be 1h, 6h, 24h, 7d or all"})
			return
		}
		start, end = now.Add(-window), now.Add(time.Minute)
	}
	empty := hostHistory{Range: rangeName, Containers: []string{}, Points: []hostHistoryPoint{}}
	if s.cfg.HostHistoryFile == "" {
		empty.Reason = "Host history is not configured for this data provider"
		s.writeJSON(w, http.StatusOK, empty)
		return
	}
	result, err := readHostHistory(s.cfg.HostHistoryFile, rangeName, start, end, now)
	if err != nil {
		s.cfg.Logger.Warn("host history unavailable", "error", err)
		empty.Reason = "Host history unavailable; the host monitor may not be running"
		s.writeJSON(w, http.StatusOK, empty)
		return
	}
	s.writeJSON(w, http.StatusOK, result)
}
