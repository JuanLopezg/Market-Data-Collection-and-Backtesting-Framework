package provider

import (
	"encoding/json"
	"io"
	"os"
	"time"
)

// A separate read-only study artifact; never merge PAPER accounts with private Kraken.
type paperComparison struct {
	ContractVersion string                    `json:"contractVersion"`
	Status          string                    `json:"status"`
	CheckedAt       string                    `json:"checkedAt"`
	Profile         string                    `json:"profile"`
	Fingerprint     string                    `json:"fingerprint"`
	Note            string                    `json:"note"`
	Rows            []paperComparisonRow      `json:"rows"`
	Positions       []paperComparisonPosition `json:"positions"`
}

type paperComparisonRow struct {
	Date              string  `json:"date"`
	LiveEquity        float64 `json:"liveEquity"`
	ExpectedEquity    float64 `json:"expectedEquity"`
	LiveCash          float64 `json:"liveCash"`
	ExpectedCash      float64 `json:"expectedCash"`
	SignalDifferences int     `json:"signalDifferences"`
}

type paperComparisonPosition struct {
	Asset            string  `json:"asset"`
	LiveQuantity     float64 `json:"liveQuantity"`
	ExpectedQuantity float64 `json:"expectedQuantity"`
	LiveUSD          float64 `json:"liveUsd"`
	ExpectedUSD      float64 `json:"expectedUsd"`
}

func readPaperComparison(path string) *paperComparison {
	missing := &paperComparison{ContractVersion: "paper-comparison-v1", Status: "UNAVAILABLE", Note: "Waiting for a completed simulated cycle and the CURRENT fast baseline.", Rows: []paperComparisonRow{}, Positions: []paperComparisonPosition{}}
	file, err := os.Open(path)
	if err != nil {
		return missing
	}
	defer file.Close()
	var value paperComparison
	if json.NewDecoder(io.LimitReader(file, 4<<20)).Decode(&value) != nil || value.ContractVersion != missing.ContractVersion {
		return missing
	}
	stamp, err := time.Parse(time.RFC3339, value.CheckedAt)
	if err != nil || time.Since(stamp) > 3*time.Minute || time.Until(stamp) > 30*time.Second {
		value.Status = "STALE"
		value.Note = "Comparison collector is stale. Retained results are historical observations. " + value.Note
	}
	return &value
}

func (cfg RealConfig) rsiThresholds() (float64, float64) {
	if cfg.RSIEntry > cfg.RSIExit && cfg.RSIExit > 0 && cfg.RSIEntry <= 100 {
		return cfg.RSIEntry, cfg.RSIExit
	}
	return pureRSIEntry, pureRSIExit
}
