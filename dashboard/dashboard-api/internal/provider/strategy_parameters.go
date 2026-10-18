package provider

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

// PureRSI parameters as the strategy service reads them from its --strategy-config file
// (config/strategies/<file>.json). The dashboard recomputes RSI and the liquidity rank with
// these values, so they must come from the same file and never from separate defaults.
type pureRSIStrategyFile struct {
	Strategies []struct {
		ID               uint64 `json:"id"`
		Type             string `json:"type"`
		MaxActiveSignals int    `json:"max_active_signals"`
		Universe         struct {
			Type      string `json:"type"`
			Count     int    `json:"count"`
			Indicator struct {
				Kind   string `json:"kind"`
				Source string `json:"source"`
				Length int    `json:"length"`
			} `json:"indicator"`
		} `json:"universe"`
		Parameters struct {
			RSILength int     `json:"rsi_length"`
			RSIEntry  float64 `json:"rsi_entry"`
			RSIExit   float64 `json:"rsi_exit"`
		} `json:"parameters"`
	} `json:"strategies"`
}

// applyStrategyConfigFile overrides the PureRSI settings of cfg with the strategy file's.
// The file is the live strategy's own configuration; a missing or unreadable file keeps
// the environment values and says so in StrategyConfigNote.
func applyStrategyConfigFile(cfg RealConfig) RealConfig {
	path := strings.TrimSpace(cfg.StrategyConfigFile)
	if path == "" {
		return cfg
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		cfg.StrategyConfigNote = "Strategy config unreadable (" + err.Error() + "); PureRSI parameters come from the dashboard environment."
		return cfg
	}
	var file pureRSIStrategyFile
	if err := json.Unmarshal(raw, &file); err != nil {
		cfg.StrategyConfigNote = "Strategy config invalid (" + err.Error() + "); PureRSI parameters come from the dashboard environment."
		return cfg
	}
	for _, strategy := range file.Strategies {
		if strategy.Type != "PureRSI" || strategy.ID != pureRSIStrategyID {
			continue
		}
		parameters := strategy.Parameters
		if parameters.RSILength <= 0 || parameters.RSIEntry <= parameters.RSIExit || parameters.RSIExit <= 0 || parameters.RSIEntry > 100 {
			cfg.StrategyConfigNote = fmt.Sprintf("Strategy config has invalid PureRSI parameters (length %d, entry %g, exit %g); dashboard environment kept.",
				parameters.RSILength, parameters.RSIEntry, parameters.RSIExit)
			return cfg
		}
		cfg.RSILength, cfg.RSIEntry, cfg.RSIExit = parameters.RSILength, parameters.RSIEntry, parameters.RSIExit
		if strategy.MaxActiveSignals > 0 {
			cfg.MaxActiveSignals = strategy.MaxActiveSignals
		}
		if strategy.Universe.Type == "top_n_liquidity" && strategy.Universe.Count > 0 {
			cfg.StrategyUniverseN = strategy.Universe.Count
		}
		if indicator := strategy.Universe.Indicator; indicator.Kind == "SMA" && indicator.Length > 0 {
			cfg.LiquidityLength = indicator.Length
			cfg.QuoteVolume = indicator.Source == "QuoteVolume"
		}
		cfg.StrategyConfigNote = "PureRSI parameters read from " + path + "."
		return cfg
	}
	cfg.StrategyConfigNote = "Strategy config has no PureRSI strategy with id 1; dashboard environment kept."
	return cfg
}

func (cfg RealConfig) rsiLength() int {
	if cfg.RSILength > 0 {
		return cfg.RSILength
	}
	return pureRSILength
}

func (cfg RealConfig) maxActiveSignals() int {
	if cfg.MaxActiveSignals > 0 {
		return cfg.MaxActiveSignals
	}
	return pureRSIMaxSignals
}

func (cfg RealConfig) liquidityLength() int {
	if cfg.LiquidityLength > 0 {
		return cfg.LiquidityLength
	}
	return pureRSILiquidityLength
}

// "SMA Quote Volume 25 (USDT)": the strategy's liquidity measure.
func (cfg RealConfig) liquidityLabel() string {
	if cfg.QuoteVolume {
		return fmt.Sprintf("SMA Quote Volume %d (USDT)", cfg.liquidityLength())
	}
	return fmt.Sprintf("SMA Volume %d", cfg.liquidityLength())
}

func (cfg RealConfig) rsiLabel() string {
	return fmt.Sprintf("RSI(%d)", cfg.rsiLength())
}
