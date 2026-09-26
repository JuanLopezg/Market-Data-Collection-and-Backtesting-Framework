package symbolregistry

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

//go:embed registry.json
var registryJSON []byte

type VenueSymbol struct {
	Venue         string `json:"venue"`
	Environment   string `json:"environment"`
	Symbol        string `json:"symbol"`
	Status        string `json:"status"`
	RoutingPolicy string `json:"routingPolicy,omitempty"`
	Reason        string `json:"reason,omitempty"`
}

type Entry struct {
	Internal   string        `json:"internal"`
	MarketData []VenueSymbol `json:"marketData"`
	Execution  []VenueSymbol `json:"execution"`
}

type Registry struct {
	SchemaVersion    int     `json:"schemaVersion"`
	RegistryVersion  string  `json:"registryVersion"`
	Policy           string  `json:"policy"`
	MarketDataSource string  `json:"marketDataSource"`
	Note             string  `json:"note"`
	Entries          []Entry `json:"entries"`
}

func Load() (Registry, string, error) {
	var registry Registry
	if err := json.Unmarshal(registryJSON, &registry); err != nil {
		return registry, "", fmt.Errorf("decode symbol registry: %w", err)
	}
	if registry.SchemaVersion != 1 {
		return registry, "", fmt.Errorf("unsupported symbol registry schemaVersion=%d", registry.SchemaVersion)
	}
	if strings.TrimSpace(registry.RegistryVersion) == "" {
		return registry, "", fmt.Errorf("symbol registry version is empty")
	}
	if registry.Policy != "EXPLICIT_ONLY" {
		return registry, "", fmt.Errorf("symbol registry policy must be EXPLICIT_ONLY")
	}
	if registry.MarketDataSource != "BINANCE" {
		return registry, "", fmt.Errorf("symbol registry marketDataSource must be BINANCE for the current project source contract")
	}
	if len(registry.Entries) == 0 {
		return registry, "", fmt.Errorf("symbol registry is empty")
	}

	seenInternal := make(map[string]struct{}, len(registry.Entries))
	for i, entry := range registry.Entries {
		if strings.TrimSpace(entry.Internal) == "" || entry.Internal != strings.TrimSpace(entry.Internal) {
			return registry, "", fmt.Errorf("entry %d has invalid internal symbol", i)
		}
		if _, exists := seenInternal[entry.Internal]; exists {
			return registry, "", fmt.Errorf("duplicate internal symbol %q", entry.Internal)
		}
		seenInternal[entry.Internal] = struct{}{}
		if len(entry.MarketData) == 0 {
			return registry, "", fmt.Errorf("internal symbol %q has no market-data mapping", entry.Internal)
		}
		if len(entry.Execution) == 0 {
			return registry, "", fmt.Errorf("internal symbol %q has no execution classification", entry.Internal)
		}
		if err := validateLegs(entry.Internal, "marketData", entry.MarketData, false); err != nil {
			return registry, "", err
		}
		if err := validateLegs(entry.Internal, "execution", entry.Execution, true); err != nil {
			return registry, "", err
		}
	}

	sum := sha256.Sum256(registryJSON)
	return registry, hex.EncodeToString(sum[:]), nil
}

func validateLegs(internal, kind string, legs []VenueSymbol, execution bool) error {
	seen := map[string]struct{}{}
	for _, leg := range legs {
		leg.Venue = strings.TrimSpace(leg.Venue)
		leg.Environment = strings.TrimSpace(leg.Environment)
		leg.Symbol = strings.TrimSpace(leg.Symbol)
		leg.Status = strings.TrimSpace(leg.Status)
		leg.RoutingPolicy = strings.TrimSpace(leg.RoutingPolicy)
		leg.Reason = strings.TrimSpace(leg.Reason)
		if leg.Venue == "" || leg.Environment == "" || leg.Status == "" {
			return fmt.Errorf("%s %q contains an incomplete %s leg", kind, internal, kind)
		}
		key := leg.Venue + "\x00" + leg.Environment
		if _, exists := seen[key]; exists {
			return fmt.Errorf("internal symbol %q has duplicate %s leg %s/%s", internal, kind, leg.Venue, leg.Environment)
		}
		seen[key] = struct{}{}

		if execution {
			switch leg.Status {
			case "MAPPED":
				if leg.Symbol == "" || leg.RoutingPolicy == "" {
					return fmt.Errorf("mapped execution leg for %q must have symbol and routingPolicy", internal)
				}
			case "BLOCKED_EXPLICIT", "DISABLED":
				if leg.RoutingPolicy != "DENY" || leg.Reason == "" {
					return fmt.Errorf("blocked execution leg for %q must use DENY and include a reason", internal)
				}
			default:
				return fmt.Errorf("unsupported execution status %q for %q", leg.Status, internal)
			}
		} else {
			if leg.Status != "SUPPORTED" || leg.Symbol == "" {
				return fmt.Errorf("market-data leg for %q must be SUPPORTED with explicit symbol", internal)
			}
		}
	}
	return nil
}

func EntryIndex(registry Registry) map[string]Entry {
	out := make(map[string]Entry, len(registry.Entries))
	for _, entry := range registry.Entries {
		out[entry.Internal] = entry
	}
	return out
}

func FindLeg(legs []VenueSymbol, venue, environment string) (VenueSymbol, bool) {
	for _, leg := range legs {
		if leg.Venue == venue && leg.Environment == environment {
			return leg, true
		}
	}
	return VenueSymbol{}, false
}

func SortedEntries(registry Registry) []Entry {
	out := append([]Entry(nil), registry.Entries...)
	sort.Slice(out, func(i, j int) bool { return out[i].Internal < out[j].Internal })
	return out
}
