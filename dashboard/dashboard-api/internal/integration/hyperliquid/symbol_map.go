package hyperliquid

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

//go:embed symbol_map.json
var symbolMapJSON []byte

type SymbolMapping struct {
	Internal string `json:"internal"`
	Venue    string `json:"venue"`
}

type UnsupportedSymbol struct {
	Internal string `json:"internal"`
	Reason   string `json:"reason"`
}

type SymbolMapManifest struct {
	SchemaVersion int                 `json:"schemaVersion"`
	Venue         string              `json:"venue"`
	Environment   string              `json:"environment"`
	MappingPolicy string              `json:"mappingPolicy"`
	Note          string              `json:"note"`
	Mappings      []SymbolMapping     `json:"mappings"`
	Unsupported   []UnsupportedSymbol `json:"unsupported"`
}

func ExplicitSymbolMap() (SymbolMapManifest, string, error) {
	var manifest SymbolMapManifest
	if err := json.Unmarshal(symbolMapJSON, &manifest); err != nil {
		return manifest, "", fmt.Errorf("decode explicit symbol map: %w", err)
	}
	if manifest.SchemaVersion != 2 || manifest.Venue != "HYPERLIQUID" || manifest.Environment != "TESTNET" || manifest.MappingPolicy != "EXPLICIT_ONLY" {
		return manifest, "", fmt.Errorf("explicit symbol map identity/policy mismatch")
	}
	seenInternal := map[string]struct{}{}
	for _, item := range manifest.Mappings {
		if strings.TrimSpace(item.Internal) == "" || strings.TrimSpace(item.Venue) == "" {
			return manifest, "", fmt.Errorf("explicit symbol map contains an empty mapping entry")
		}
		if item.Internal != strings.TrimSpace(item.Internal) || item.Venue != strings.TrimSpace(item.Venue) {
			return manifest, "", fmt.Errorf("explicit symbol map contains non-canonical whitespace")
		}
		if _, exists := seenInternal[item.Internal]; exists {
			return manifest, "", fmt.Errorf("duplicate internal symbol %q", item.Internal)
		}
		seenInternal[item.Internal] = struct{}{}
	}
	for _, item := range manifest.Unsupported {
		if strings.TrimSpace(item.Internal) == "" || strings.TrimSpace(item.Reason) == "" {
			return manifest, "", fmt.Errorf("explicit symbol map contains an empty unsupported entry")
		}
		if item.Internal != strings.TrimSpace(item.Internal) || item.Reason != strings.TrimSpace(item.Reason) {
			return manifest, "", fmt.Errorf("explicit unsupported symbol contains non-canonical whitespace")
		}
		if _, exists := seenInternal[item.Internal]; exists {
			return manifest, "", fmt.Errorf("internal symbol %q appears in more than one mapping classification", item.Internal)
		}
		seenInternal[item.Internal] = struct{}{}
	}
	sum := sha256.Sum256(symbolMapJSON)
	return manifest, hex.EncodeToString(sum[:]), nil
}

func MappingIndex(manifest SymbolMapManifest) map[string]string {
	result := make(map[string]string, len(manifest.Mappings))
	for _, item := range manifest.Mappings {
		result[item.Internal] = item.Venue
	}
	return result
}

func UnsupportedIndex(manifest SymbolMapManifest) map[string]string {
	result := make(map[string]string, len(manifest.Unsupported))
	for _, item := range manifest.Unsupported {
		result[item.Internal] = item.Reason
	}
	return result
}

func SortedMappings(manifest SymbolMapManifest) []SymbolMapping {
	out := append([]SymbolMapping(nil), manifest.Mappings...)
	sort.Slice(out, func(i, j int) bool { return out[i].Internal < out[j].Internal })
	return out
}
