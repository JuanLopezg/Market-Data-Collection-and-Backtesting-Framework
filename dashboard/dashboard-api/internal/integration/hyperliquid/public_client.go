package hyperliquid

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

const maxPublicResponseBytes = 4 << 20

type PublicClient struct {
	infoURL string
	http    *http.Client
}

type PublicCheck struct {
	Connected       bool
	MetadataReady   bool
	MidsReady       bool
	UniverseCount   int
	MidCount        int
	MatchedMidCount int
	SampleSymbols   []string
	Latency         time.Duration
	CheckedAt       time.Time
}

type metaResponse struct {
	Universe []struct {
		Name         string `json:"name"`
		SzDecimals   int    `json:"szDecimals"`
		MaxLeverage  int    `json:"maxLeverage"`
		OnlyIsolated bool   `json:"onlyIsolated"`
		IsDelisted   bool   `json:"isDelisted"`
		MarginMode   string `json:"marginMode"`
	} `json:"universe"`
}

func NewPublicClient(infoURL string, timeout time.Duration) *PublicClient {
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	return &PublicClient{
		infoURL: strings.TrimSpace(infoURL),
		http: &http.Client{
			Timeout: timeout,
			CheckRedirect: func(_ *http.Request, _ []*http.Request) error {
				return fmt.Errorf("redirects are disabled for venue public probes")
			},
		},
	}
}

func (c *PublicClient) Check(ctx context.Context) (PublicCheck, error) {
	started := time.Now()
	result := PublicCheck{CheckedAt: time.Now().UTC()}
	if c == nil || c.infoURL == "" {
		return result, fmt.Errorf("Hyperliquid public info URL is not configured")
	}

	var meta metaResponse
	if err := c.postInfo(ctx, map[string]any{"type": "meta"}, &meta); err != nil {
		return result, fmt.Errorf("Hyperliquid public metadata probe failed: %w", err)
	}
	if len(meta.Universe) == 0 {
		return result, fmt.Errorf("Hyperliquid public metadata returned an empty perpetual universe")
	}
	result.MetadataReady = true
	result.UniverseCount = len(meta.Universe)

	mids := map[string]string{}
	if err := c.postInfo(ctx, map[string]any{"type": "allMids"}, &mids); err != nil {
		return result, fmt.Errorf("Hyperliquid public mids probe failed: %w", err)
	}
	if len(mids) == 0 {
		return result, fmt.Errorf("Hyperliquid public mids returned no assets")
	}
	result.MidsReady = true
	result.MidCount = len(mids)

	for _, asset := range meta.Universe {
		name := strings.TrimSpace(asset.Name)
		if name == "" {
			continue
		}
		if len(result.SampleSymbols) < 8 {
			result.SampleSymbols = append(result.SampleSymbols, name)
		}
		if value, ok := mids[name]; ok && strings.TrimSpace(value) != "" {
			result.MatchedMidCount++
		}
	}
	if result.MatchedMidCount == 0 {
		return result, fmt.Errorf("Hyperliquid metadata and mids had no overlapping perpetual asset names")
	}

	result.Connected = true
	result.Latency = time.Since(started)
	return result, nil
}

func (c *PublicClient) postInfo(ctx context.Context, payload map[string]any, target any) error {
	encoded, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("encode request: %w", err)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.infoURL, bytes.NewReader(encoded))
	if err != nil {
		return fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "control-dashboard-public-probe/0.43.0")

	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	decoder := json.NewDecoder(io.LimitReader(resp.Body, maxPublicResponseBytes))
	if err := decoder.Decode(target); err != nil {
		return fmt.Errorf("decode JSON response: %w", err)
	}
	return nil
}

// Universe returns the exact Hyperliquid coin names published by the public
// TESTNET meta endpoint. It does not infer or transform any internal symbol.
func (c *PublicClient) Universe(ctx context.Context) ([]string, time.Duration, time.Time, error) {
	started := time.Now()
	checkedAt := time.Now().UTC()
	if c == nil || c.infoURL == "" {
		return nil, 0, checkedAt, fmt.Errorf("Hyperliquid public info URL is not configured")
	}
	var meta metaResponse
	if err := c.postInfo(ctx, map[string]any{"type": "meta"}, &meta); err != nil {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata probe failed: %w", err)
	}
	if len(meta.Universe) == 0 {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata returned an empty perpetual universe")
	}
	out := make([]string, 0, len(meta.Universe))
	seen := map[string]struct{}{}
	for _, asset := range meta.Universe {
		name := strings.TrimSpace(asset.Name)
		if name == "" {
			continue
		}
		if _, ok := seen[name]; ok {
			continue
		}
		seen[name] = struct{}{}
		out = append(out, name)
	}
	if len(out) == 0 {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata contained no usable coin names")
	}
	return out, time.Since(started), checkedAt, nil
}
