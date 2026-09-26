package hyperliquid

import (
	"context"
	"fmt"
	"strings"
	"time"
)

// PublicPerpRule is the subset of public Hyperliquid perpetual metadata needed
// by Step 36. Mid is public market data and is used only to estimate the
// minimum quantity that satisfies the documented minimum order notional.
// Nothing in this type is private account state or an order-routing contract.
type PublicPerpRule struct {
	Name         string
	SzDecimals   int
	MaxLeverage  int
	OnlyIsolated bool
	IsDelisted   bool
	MarginMode   string
	Mid          string
}

// PerpRules reads only public TESTNET /info payloads: meta + allMids.
// It performs public reads only; no private or mutating exchange action exists.
func (c *PublicClient) PerpRules(ctx context.Context) ([]PublicPerpRule, time.Duration, time.Time, error) {
	started := time.Now()
	checkedAt := time.Now().UTC()
	if c == nil || c.infoURL == "" {
		return nil, 0, checkedAt, fmt.Errorf("Hyperliquid public info URL is not configured")
	}

	var meta metaResponse
	if err := c.postInfo(ctx, map[string]any{"type": "meta"}, &meta); err != nil {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata/rules probe failed: %w", err)
	}
	if len(meta.Universe) == 0 {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata returned an empty perpetual universe")
	}

	mids := map[string]string{}
	if err := c.postInfo(ctx, map[string]any{"type": "allMids"}, &mids); err != nil {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public mids/rules probe failed: %w", err)
	}
	if len(mids) == 0 {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public mids returned no assets")
	}

	out := make([]PublicPerpRule, 0, len(meta.Universe))
	seen := map[string]struct{}{}
	for _, asset := range meta.Universe {
		name := strings.TrimSpace(asset.Name)
		if name == "" {
			continue
		}
		if _, exists := seen[name]; exists {
			return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid metadata contains duplicate perpetual coin %q", name)
		}
		seen[name] = struct{}{}
		out = append(out, PublicPerpRule{
			Name:         name,
			SzDecimals:   asset.SzDecimals,
			MaxLeverage:  asset.MaxLeverage,
			OnlyIsolated: asset.OnlyIsolated,
			IsDelisted:   asset.IsDelisted,
			MarginMode:   strings.TrimSpace(asset.MarginMode),
			Mid:          strings.TrimSpace(mids[name]),
		})
	}
	if len(out) == 0 {
		return nil, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid public metadata contained no usable perpetual rules")
	}
	return out, time.Since(started), checkedAt, nil
}
