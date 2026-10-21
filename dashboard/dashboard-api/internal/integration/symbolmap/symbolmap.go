// Package symbolmap reads the Binance -> venue symbol table that the exchange gateway uses
// (config/venues/<venue>/symbol_map.json, built by tools/kraken_symbol_map.py).
//
// Each entry gives the venue contract and the unit scale:
//
//	venue quantity = source quantity x VenueUnitsPerSourceUnit
//	venue price    = source price    / VenueUnitsPerSourceUnit
//
// A source symbol missing from the table is not tradeable on that venue.
package symbolmap

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strings"
)

type Entry struct {
	Source                  string  `json:"source"`
	Asset                   string  `json:"asset"`
	VenueSymbol             string  `json:"venue_symbol"`
	VenueUnitsPerSourceUnit float64 `json:"venue_units_per_source_unit"`
}

type Table struct {
	SchemaVersion int     `json:"schema_version"`
	Venue         string  `json:"venue"`
	GeneratedAt   string  `json:"generated_at"`
	Symbols       []Entry `json:"symbols"`
	// Venue contracts the tool saw and deliberately left out. nil when the table predates
	// this list: new venue contracts cannot be told apart then.
	VenueUnmapped []string `json:"kraken_unmapped"`
	bySource      map[string]Entry
	byVenue       map[string]Entry
}

// Load reads and validates the table: one entry per source and per venue symbol,
// non-empty names and a positive scale. Same rules as the gateway's SymbolMap.
func Load(path, expectedVenue string) (Table, error) {
	var table Table
	raw, err := os.ReadFile(path)
	if err != nil {
		return table, fmt.Errorf("read symbol map: %w", err)
	}
	if err := json.Unmarshal(raw, &table); err != nil {
		return table, fmt.Errorf("decode symbol map: %w", err)
	}
	if table.SchemaVersion != 1 {
		return table, fmt.Errorf("symbol map schema_version %d is not supported", table.SchemaVersion)
	}
	if table.Venue != expectedVenue {
		return table, fmt.Errorf("symbol map is for venue %q, expected %q", table.Venue, expectedVenue)
	}
	table.bySource = make(map[string]Entry, len(table.Symbols))
	table.byVenue = make(map[string]Entry, len(table.Symbols))
	for i, entry := range table.Symbols {
		if strings.TrimSpace(entry.Source) == "" || strings.TrimSpace(entry.Asset) == "" || strings.TrimSpace(entry.VenueSymbol) == "" {
			return table, fmt.Errorf("symbol map entry %d has an empty field", i)
		}
		if math.IsNaN(entry.VenueUnitsPerSourceUnit) || math.IsInf(entry.VenueUnitsPerSourceUnit, 0) || entry.VenueUnitsPerSourceUnit <= 0 {
			return table, fmt.Errorf("symbol map scale for %s must be positive", entry.Source)
		}
		if _, duplicate := table.bySource[entry.Source]; duplicate {
			return table, fmt.Errorf("symbol map lists %s twice", entry.Source)
		}
		if _, duplicate := table.byVenue[entry.VenueSymbol]; duplicate {
			return table, fmt.Errorf("symbol map maps two source symbols to %s", entry.VenueSymbol)
		}
		table.bySource[entry.Source] = entry
		table.byVenue[entry.VenueSymbol] = entry
	}
	return table, nil
}

// BySource returns the entry for a Binance symbol such as "1000PEPEUSDT".
func (t Table) BySource(source string) (Entry, bool) {
	entry, ok := t.bySource[source]
	return entry, ok
}

// ByVenue returns the entry for a venue contract such as "PF_PEPEUSD".
func (t Table) ByVenue(venueSymbol string) (Entry, bool) {
	entry, ok := t.byVenue[venueSymbol]
	return entry, ok
}
