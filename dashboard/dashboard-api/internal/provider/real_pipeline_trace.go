package provider

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

// The full Trace of one asset's daily cycle: every step from the market close to the
// exchange, each with the numbers and the rule that produced it. Durable checkpoints give
// the signals, risk evaluation, plan and orders; indicators and the liquidity rank are
// recomputed from the canonical market SQLite with the live strategy's own parameters
// (see applyStrategyConfigFile), as the Market Data page does.

// portfolioRules is the portfolio-risk configuration the decision was made with (the
// --portfolio-config JSON persisted by the risk service).
type portfolioRules struct {
	Strategies []struct {
		ID    uint64 `json:"id"`
		Sizer struct {
			Type                string  `json:"type"`
			WeightPerFullSignal float64 `json:"weight_per_full_signal"`
			TargetVolatility    float64 `json:"target_volatility"`
			CovarianceLookback  int     `json:"covariance_lookback"`
		} `json:"sizer"`
		Rebalance struct {
			Type      string  `json:"type"`
			Threshold float64 `json:"threshold"`
		} `json:"rebalance"`
	} `json:"strategies"`
}

func parsePortfolioRules(checkpoint pgstore.PipelineCheckpoint) (portfolioRules, bool) {
	var rules portfolioRules
	candidates := []string{checkpoint.PortfolioConfig}
	if checkpoint.RiskDiagnostics != nil {
		// The identity is the portfolio JSON followed by "\n\nmarket_data_mode=...".
		identity := checkpoint.RiskDiagnostics.ConfigurationIdentity
		if cut := strings.Index(identity, "\n\n"); cut > 0 {
			identity = identity[:cut]
		}
		candidates = append(candidates, identity)
	}
	for _, text := range candidates {
		if strings.TrimSpace(text) != "" && json.Unmarshal([]byte(text), &rules) == nil && len(rules.Strategies) > 0 {
			return rules, true
		}
	}
	return portfolioRules{}, false
}

type pipelineTraceInputs struct {
	cfg       RealConfig
	market    map[string]marketAssetView // nil when the market SQLite was unreadable
	marketErr error
	fills     []tradingwire.Fill // fills of this cycle's planned orders
	exchange  string
}

// enrichPipeline completes the matrix rows and traces built from the checkpoints.
func enrichPipeline(data *realPipelineData, checkpoint pgstore.PipelineCheckpoint, inputs pipelineTraceInputs) {
	rules, rulesKnown := parsePortfolioRules(checkpoint)
	for index := range data.Rows {
		row := &data.Rows[index]
		trace := data.Traces[row.CycleID]
		view, viewKnown := inputs.market[row.Asset]
		viewAligned := viewKnown && view.latestDate == checkpoint.Timestamp

		// Market: liquidity rank inside the strategy's ranking.
		trace.Market.Universe = fmt.Sprintf("%s · strategy universe: top %d by %s", trace.Market.Universe, inputs.cfg.StrategyUniverseN, inputs.cfg.liquidityLabel())
		switch {
		case viewKnown:
			row.RankLabel = fmt.Sprintf("#%d", view.rank)
			membership := "inside"
			if view.rank > view.universeN {
				membership = "outside"
				row.RankLabel += " (out)"
			}
			trace.Market.LiquidityRank = fmt.Sprintf("#%d of %d by %s = %s USDT · %s the strategy top %d",
				view.rank, view.candidates, inputs.cfg.liquidityLabel(), formatCompactVolume(view.smaVolume), membership, view.universeN)
			if !viewAligned {
				trace.Market.LiquidityRank += fmt.Sprintf(" (market data of %s, decision %s)", formatMarketDate(view.latestDate), formatTradingTimestamp(checkpoint.Timestamp))
			}
		case inputs.marketErr != nil:
			trace.Market.LiquidityRank = "Market SQLite unreadable: " + inputs.marketErr.Error()
		default:
			row.RankLabel = "not ranked"
			trace.Market.LiquidityRank = "Not in the ranked candidates of the latest market day (no candle or no " + inputs.cfg.liquidityLabel() + ")"
		}

		// Strategy: indicator, rule and transition.
		entry, exit := inputs.cfg.rsiThresholds()
		rsiLabel := inputs.cfg.rsiLabel()
		trace.Strategy.Rule = fmt.Sprintf("PureRSI: enter when %s > %g, exit when %s < %g; at most %d signals, ranked by %s",
			rsiLabel, entry, rsiLabel, exit, inputs.cfg.maxActiveSignals(), inputs.cfg.liquidityLabel())
		rsi := math.NaN()
		if viewKnown && isFinite(view.rsi) {
			rsi = view.rsi
			trace.Strategy.RSI = fmt.Sprintf("%s = %.1f at the %s close", rsiLabel, view.rsi, formatMarketDate(view.latestDate))
			row.RSI, row.RSILabel = view.rsi, fmt.Sprintf("%.1f", view.rsi)
		} else if viewKnown {
			trace.Strategy.RSI = "Not enough history to compute " + rsiLabel
		} else {
			trace.Strategy.RSI = rsiLabel + " not available (asset not in the ranked market data)"
		}
		trace.Strategy.Transition = signalTransition(checkpoint, row.Asset, rsi, viewAligned, entry, exit, rsiLabel)
		for stage := range row.Stages {
			if row.Stages[stage].ID == "indicators" {
				row.Stages[stage] = realPipelineStage{ID: "indicators", Label: "Indicators", Value: trace.Strategy.RSI,
					Detail: trace.Strategy.Rule + "; recomputed from the canonical market SQLite.", State: stageState(isFinite(rsi), "OK", "PENDING")}
			}
		}

		// Portfolio / risk: sizer, caps, rebalance band, approved target.
		explainPortfolio(&trace, row, checkpoint, rules, rulesKnown)

		// Execution: fees and slippage of this cycle's fills.
		explainExecution(&trace, row.Asset, inputs.fills, checkpoint)

		// Audit.
		if checkpoint.RiskDiagnostics != nil && checkpoint.RiskDiagnostics.ConfigurationFingerprint != "" {
			trace.Audit.ConfigVersion = "risk config " + checkpoint.RiskDiagnostics.ConfigurationFingerprint
		}
		if inputs.cfg.StrategyConfigFile != "" {
			trace.Audit.ConfigVersion += " · strategy " + inputs.cfg.StrategyConfigFile
		}
		trace.Audit.Exchange = inputs.exchange
		data.Traces[row.CycleID] = trace
	}
	if inputs.cfg.StrategyConfigNote != "" {
		data.SourceNote += " " + inputs.cfg.StrategyConfigNote
	}
}

func signalOf(batch *tradingwire.StrategyIntentBatch, asset string) (float64, bool) {
	if batch == nil {
		return 0, false
	}
	total, found := 0.0, false
	for _, strategy := range batch.Strategies {
		if value, ok := strategy.Signals[asset]; ok {
			total, found = total+value, true
		}
	}
	return total, found
}

func signalWord(value float64) string {
	switch {
	case value > 1e-12:
		return "LONG"
	case value < -1e-12:
		return "SHORT"
	}
	return "FLAT"
}

// signalTransition explains the signal against the previous decision: an entry, a hold or
// an exit, with the RSI rule that applies.
func signalTransition(checkpoint pgstore.PipelineCheckpoint, asset string, rsi float64, rsiAligned bool, entry, exit float64, rsiLabel string) string {
	now, _ := signalOf(&checkpoint.Signals, asset)
	if checkpoint.PreviousSignals == nil {
		return "First decision checkpoint: no previous signal to compare"
	}
	before, _ := signalOf(checkpoint.PreviousSignals, asset)
	prefix := fmt.Sprintf("%s (%s) → %s (%s)", signalWord(before), formatTradingTimestamp(checkpoint.PreviousSignals.Timestamp),
		signalWord(now), formatTradingTimestamp(checkpoint.Timestamp))
	rsiText := ""
	if rsiAligned && isFinite(rsi) {
		rsiText = fmt.Sprintf("%s %.1f", rsiLabel, rsi)
	}
	switch {
	case before == 0 && now > 0:
		if rsiText != "" {
			return fmt.Sprintf("%s: entry, %s > %g", prefix, rsiText, entry)
		}
		return prefix + ": entry"
	case before > 0 && now > 0:
		if rsiText != "" {
			return fmt.Sprintf("%s: held, %s is not below the exit %g", prefix, rsiText, exit)
		}
		return prefix + ": held"
	case before > 0 && now == 0:
		if rsiText != "" && rsi < exit {
			return fmt.Sprintf("%s: exit, %s < %g", prefix, rsiText, exit)
		}
		return prefix + ": exit (signal dropped: exit rule or left the universe)"
	case before == 0 && now == 0:
		if rsiText != "" && rsi > entry {
			return fmt.Sprintf("%s: no entry although %s > %g (signal cap or ranking)", prefix, rsiText, entry)
		}
		return prefix + ": no entry"
	}
	return prefix
}

func explainPortfolio(trace *realWhyTrace, row *realPipelineAssetRow, checkpoint pgstore.PipelineCheckpoint, rules portfolioRules, rulesKnown bool) {
	if checkpoint.RiskDiagnostics == nil {
		return
	}
	for _, strategy := range checkpoint.RiskDiagnostics.Strategies {
		evaluated, ok := strategy.Assets[row.Asset]
		if !ok || evaluated.SizedWeight == nil || evaluated.AssetCappedWeight == nil || evaluated.ApprovedWeight == nil {
			continue
		}
		signal := 0.0
		for _, intent := range checkpoint.Signals.Strategies {
			if intent.StrategyID == strategy.StrategyID {
				signal = intent.Signals[row.Asset]
			}
		}
		sized, capped, approved := *evaluated.SizedWeight, *evaluated.AssetCappedWeight, *evaluated.ApprovedWeight
		capital := strategy.ReferenceCapital
		var rule struct {
			sizer, rebalance string
			weight, target   float64
			threshold        float64
		}
		if rulesKnown {
			for _, configured := range rules.Strategies {
				if configured.ID == strategy.StrategyID {
					rule.sizer, rule.weight, rule.target = configured.Sizer.Type, configured.Sizer.WeightPerFullSignal, configured.Sizer.TargetVolatility
					rule.rebalance, rule.threshold = configured.Rebalance.Type, configured.Rebalance.Threshold
				}
			}
		}

		// Sizing: equal weight or volatility target, as configured.
		switch {
		case strategy.Sizer == "EqualWeight" || rule.sizer == "equal_weight":
			trace.Portfolio.Sizer = fmt.Sprintf("Equal weight: %s of strategy capital per full signal", formatPercent(rule.weight*100))
			trace.Portfolio.RawTarget = fmt.Sprintf("signal %s × %s = %s of %s = %s", formatSignal(signal), formatPercent(rule.weight*100),
				formatPercent(sized*100), formatDollars(capital), formatDollars(sized*capital))
			trace.Portfolio.VolTarget = ""
		default:
			trace.Portfolio.Sizer = fmt.Sprintf("Volatility target %s (%s)", formatPercent(rule.target*100), strategy.Sizer)
			trace.Portfolio.RawTarget = fmt.Sprintf("%s of %s = %s", formatPercent(sized*100), formatDollars(capital), formatDollars(sized*capital))
			if volatility := strategy.Volatility; volatility != nil {
				trace.Portfolio.VolTarget = fmt.Sprintf("book volatility %s at equal signals → scale %.2f to reach %s; after caps %s",
					formatPercent(volatility.RawSignal*100), volatility.Scale, formatPercent(volatility.Target*100), formatPercent(volatility.AfterConstraints*100))
			} else {
				trace.Portfolio.VolTarget = "Volatility state " + strategy.VolatilityState
			}
		}

		// Risk caps, in the order the risk engine applies them.
		changes := []string{
			fmt.Sprintf("Asset cap %s: %s → %s%s", formatPercent(strategy.MaxAssetWeight*100), formatPercent(sized*100), formatPercent(capped*100),
				map[bool]string{true: " (no change)", false: " (reduced)"}[sized == capped]),
			fmt.Sprintf("Gross cap %s: book gross %s → scale %.2f → %s", formatPercent(strategy.MaxGrossLeverage*100),
				formatPercent(strategy.GrossAfterAssetCap*100), strategy.GrossScale, formatPercent(approved*100)),
		}

		// Rebalance band: why the position is resized or held.
		currentQty := 0.0
		if evaluated.CurrentQuantity != nil {
			currentQty = *evaluated.CurrentQuantity
		}
		close := closeOf(checkpoint, row.Asset)
		rebalance := ""
		switch {
		case evaluated.Action == "FLAT":
			rebalance = "Exit: the approved weight is 0, the position is closed"
		case currentQty == 0 && evaluated.Action == "TARGET_WEIGHT":
			rebalance = "Entry: no position yet, the approved weight is bought"
		case close > 0 && capital > 0 && currentQty != 0:
			current := currentQty * close / capital
			diff := math.Abs(approved - current)
			moved := evaluated.Action == "TARGET_WEIGHT"
			weights := fmt.Sprintf("current %s (%s × %s / %s) vs target %s, difference %.2f pp",
				formatPercent(current*100), formatQuantity(currentQty), formatPriceUSD(close), formatDollars(capital), formatPercent(approved*100), diff*100)
			switch rule.rebalance {
			case "relative_threshold":
				band := rule.threshold * math.Abs(current)
				rebalance = fmt.Sprintf("Relative band %s: %s; band %s × %s = %.2f pp → %s", formatPercent(rule.threshold*100), weights,
					formatPercent(rule.threshold*100), formatPercent(current*100), band*100, map[bool]string{true: "RESIZE", false: "HOLD"}[moved])
			case "threshold":
				rebalance = fmt.Sprintf("Absolute band %.2f pp: %s → %s", rule.threshold*100, weights, map[bool]string{true: "RESIZE", false: "HOLD"}[moved])
			case "entry_exit_only":
				rebalance = "Entry/exit only: held positions are never resized; " + weights
			default:
				rebalance = weights + " → " + evaluated.Action
			}
		default:
			rebalance = "Action " + evaluated.Action
		}
		trace.Portfolio.Rebalance = rebalance
		trace.Portfolio.RiskChanges = changes

		target := approved * capital
		switch evaluated.Action {
		case "HOLD":
			trace.Portfolio.ApprovedTarget = fmt.Sprintf("HOLD at %s approved (%s): keeps %s, no order", formatPercent(approved*100), formatDollars(target), signedQuantity(currentQty))
			if !pipelineHasTarget(checkpoint, row.Asset) {
				row.TargetLabel = formatDollars(target) + " (hold)"
			}
		case "FLAT":
			trace.Portfolio.ApprovedTarget = "FLAT: target 0, the position is sold"
		default:
			trace.Portfolio.ApprovedTarget = fmt.Sprintf("TARGET %s of %s = %s", formatPercent(approved*100), formatDollars(capital), formatDollars(target))
		}
		return
	}
}

func closeOf(checkpoint pgstore.PipelineCheckpoint, asset string) float64 {
	if checkpoint.PlanningRequest != nil {
		if close, ok := checkpoint.PlanningRequest.ReferenceCloses.Closes[asset]; ok {
			return close
		}
	}
	return 0
}

func explainExecution(trace *realWhyTrace, asset string, fills []tradingwire.Fill, checkpoint pgstore.PipelineCheckpoint) {
	quantity, notional, fees := 0.0, 0.0, 0.0
	count := 0
	for _, fill := range fills {
		if fill.Coin != asset {
			continue
		}
		count++
		quantity += fill.Quantity
		notional += fill.Quantity * fill.Price
		fees += fill.Commission
	}
	if count == 0 {
		trace.Execution.Fees = "No fill this cycle"
		trace.Execution.Slippage = "No fill this cycle"
		return
	}
	trace.Execution.Fees = fmt.Sprintf("%s over %d fill(s)", formatDollars(fees), count)
	close := closeOf(checkpoint, asset)
	if close <= 0 || quantity <= 0 {
		trace.Execution.Slippage = "Decision close not available"
		return
	}
	average := notional / quantity
	trace.Execution.Slippage = fmt.Sprintf("average fill %s vs decision close %s: %+.2f%%", formatPriceUSD(average), formatPriceUSD(close), (average/close-1)*100)
}
