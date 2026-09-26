package hyperliquid

import (
	"context"
	"fmt"
	"strings"
	"time"
)

// UserRole is the public /info userRole response used by Step 37 to verify that
// an API wallet address is currently registered as an agent for the configured
// TESTNET account. This endpoint does not require or receive any private key.
type UserRole struct {
	Role string `json:"role"`
	Data struct {
		User   string `json:"user,omitempty"`
		Master string `json:"master,omitempty"`
	} `json:"data,omitempty"`
}

func (c *PublicClient) UserRole(ctx context.Context, address string) (UserRole, time.Duration, time.Time, error) {
	started := time.Now()
	checkedAt := time.Now().UTC()
	var result UserRole
	address = strings.ToLower(strings.TrimSpace(address))
	if c == nil || c.infoURL == "" {
		return result, 0, checkedAt, fmt.Errorf("Hyperliquid public info URL is not configured")
	}
	if address == "" {
		return result, 0, checkedAt, fmt.Errorf("Hyperliquid address is empty")
	}
	if err := c.postInfo(ctx, map[string]any{"type": "userRole", "user": address}, &result); err != nil {
		return result, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid userRole probe failed: %w", err)
	}
	result.Role = strings.TrimSpace(result.Role)
	result.Data.User = strings.ToLower(strings.TrimSpace(result.Data.User))
	result.Data.Master = strings.ToLower(strings.TrimSpace(result.Data.Master))
	if result.Role == "" {
		return result, time.Since(started), checkedAt, fmt.Errorf("Hyperliquid userRole response omitted role")
	}
	return result, time.Since(started), checkedAt, nil
}
