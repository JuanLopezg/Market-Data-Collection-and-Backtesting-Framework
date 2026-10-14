// Package kraken reads public Kraken Futures market data. No keys, GET only.
package kraken

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// TickersURL is the production public endpoint; there is no Kraken Futures demo in use.
const TickersURL = "https://futures.kraken.com/derivatives/api/v3/tickers"

const maxResponseBytes = 8 << 20

// Ticker is one Multi-M perpetual: the mark price is USD per venue unit.
type Ticker struct {
	Symbol    string
	MarkPrice float64
	Suspended bool
}

type PublicClient struct {
	tickersURL string
	client     *http.Client
}

// NewPublicClient takes the full tickers URL, e.g.
// https://futures.kraken.com/derivatives/api/v3/tickers. An empty URL disables reads.
func NewPublicClient(tickersURL string, timeout time.Duration) *PublicClient {
	return &PublicClient{tickersURL: strings.TrimSpace(tickersURL), client: &http.Client{
		Timeout: timeout,
		// Fixed public host: never follow a redirect elsewhere.
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}}
}

// Tickers returns the PF_ perpetual tickers by venue symbol.
func (c *PublicClient) Tickers(ctx context.Context) (map[string]Ticker, error) {
	if c == nil || c.tickersURL == "" {
		return nil, errors.New("Kraken public tickers URL is not configured")
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, c.tickersURL, nil)
	if err != nil {
		return nil, fmt.Errorf("build Kraken tickers request: %w", err)
	}
	request.Header.Set("Accept", "application/json")
	request.Header.Set("User-Agent", "algoTrading-dashboard")
	response, err := c.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("Kraken tickers request failed: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("Kraken tickers returned HTTP %d", response.StatusCode)
	}

	var body struct {
		Result  string `json:"result"`
		Tickers []struct {
			Symbol    string   `json:"symbol"`
			MarkPrice *float64 `json:"markPrice"`
			Suspended bool     `json:"suspended"`
		} `json:"tickers"`
	}
	if err := json.NewDecoder(io.LimitReader(response.Body, maxResponseBytes)).Decode(&body); err != nil {
		return nil, fmt.Errorf("decode Kraken tickers: %w", err)
	}
	if body.Result != "success" {
		return nil, fmt.Errorf("Kraken tickers result %q", body.Result)
	}
	tickers := make(map[string]Ticker, len(body.Tickers))
	for _, row := range body.Tickers {
		if !strings.HasPrefix(row.Symbol, "PF_") {
			continue
		}
		ticker := Ticker{Symbol: row.Symbol, Suspended: row.Suspended}
		if row.MarkPrice != nil {
			ticker.MarkPrice = *row.MarkPrice
		}
		tickers[row.Symbol] = ticker
	}
	if len(tickers) == 0 {
		return nil, errors.New("Kraken tickers listed no PF_ perpetuals")
	}
	return tickers, nil
}
