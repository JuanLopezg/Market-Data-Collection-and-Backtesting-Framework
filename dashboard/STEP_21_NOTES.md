# Step 21 — Real Risk

Step 21 enables `/api/risk` in `DASHBOARD_DATA_PROVIDER=real`.

Canonical source: `portfolio_risk_live_decision_checkpoint`, read through the existing cycle-aligned PostgreSQL adapter. The checkpoint provides the exact persisted StrategyIntentBatch and AccountSnapshot inputs plus the approved DecisionBatch output.

The page exposes:
- decision timestamp/correlation/message IDs,
- account cash input,
- strategy reference capital,
- approved target notional and approved target weights,
- approved gross/net target ratios,
- input signal direction and strategy IDs.

Fail-closed / explicitly unavailable:
- active runtime portfolio config/version and limit values,
- raw pre-risk target,
- binding constraint or clipping reason,
- volatility/covariance scaling diagnostics,
- current marked portfolio weights/PnL,
- durable breach/warning report,
- kill switch and global trading-readiness state.

The dashboard never recomputes those missing diagnostics and labels the result canonical.
