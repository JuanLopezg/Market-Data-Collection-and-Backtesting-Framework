import { AlertTriangle, LockKeyhole, Pause, Play, ShieldAlert, ShieldCheck, Skull } from 'lucide-react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { RealRiskAsset, RiskData, RiskLimitState } from '../types/dashboard'

function RiskState({ state }: { state: RiskLimitState }) {
  const tone = state === 'OK' ? 'good' : state === 'WARN' ? 'warn' : 'bad'
  return <span className={`risk-state risk-state--${tone}`}>{state}</span>
}

function Utilization({ value, state }: { value: number; state: RiskLimitState }) {
  return <div className="utilization"><div className="utilization__track"><span className={`utilization__fill utilization__fill--${state.toLowerCase()}`} style={{ width: `${Math.min(value, 100)}%` }}/></div><strong>{value.toFixed(0)}%</strong></div>
}

function signalClass(signal: RealRiskAsset['signal']) {
  return signal === 'LONG' ? 'positive' : signal === 'SHORT' ? 'negative' : 'muted'
}

function RealRiskView({ data }: { data: RiskData }) {
  const assets = data.realAssets ?? []
  const missing = data.missingDiagnostics ?? []
  const snapshot = data.evidenceKind === 'REPLAY_SNAPSHOT'

  return <>
    <div className="page-heading">
      <div><h1>Risk & Safety</h1><p>{snapshot ? 'Canonical MOCK account observations and risk diagnostic availability.' : 'Persisted PortfolioRisk decision boundary and input evidence.'}</p></div>
      <div className="cycle-label"><ShieldCheck size={14}/><span>{data.riskState}</span></div>
    </div>

    <div className="real-source-banner risk-source-note"><strong>{snapshot ? 'MOCK REPLAY · account observation' : 'REAL DATA'}</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Observed ${data.sourceUpdatedAt}.` : ''}</span></div>
    {data.decisionDetail && <div className="callout callout--info"><strong>{data.riskState}</strong><span>{data.decisionDetail}</span></div>}

    <section className="risk-summary">
      <div className="mini-metric"><span>Decision</span><strong>{data.decisionTimestamp ?? '—'}</strong><small>{snapshot ? 'Risk decision not exposed by this snapshot' : 'PortfolioRisk durable checkpoint'}</small></div>
      <div className="mini-metric"><span>{snapshot ? 'Marked account equity' : 'Emitted decision reference capital'}</span><strong>{data.referenceCapitalLabel ?? '—'}</strong><small>{snapshot ? 'Canonical MOCK account snapshot' : `${data.strategyCount ?? 0} input strategy batch(es)`}</small></div>
      <div className="mini-metric"><span>Approved Gross Target</span><strong>{data.approvedGrossTargetLabel ?? '—'}</strong><small>{data.approvedTargetNotionalLabel ?? '—'} absolute target notional</small></div>
      <div className="mini-metric"><span>Approved Net Target</span><strong>{data.approvedNetTargetLabel ?? '—'}</strong><small>Signed approved notional / reference capital</small></div>
    </section>

    <section className="risk-grid">
      <Panel title={snapshot ? 'Observed Account' : 'Persisted Decision Boundary'} right={<span className="muted">{snapshot ? 'Snapshot, not decision lineage' : 'Canonical checkpoint'}</span>}>
        <div className="key-value-grid">
          <span>Account cash input</span><strong>{data.accountCashLabel ?? '—'}</strong>
          <span>Approved target assets</span><strong>{snapshot || data.evidenceKind === 'PENDING' ? 'Not exposed' : data.activeTargetCount ?? 0}</strong>
          <span>Non-zero input signals</span><strong>{snapshot || data.evidenceKind === 'PENDING' ? 'Not exposed' : data.nonZeroSignalCount ?? 0}</strong>
          <span>Decision correlation</span><code>{data.decisionCorrelationId || '—'}</code>
          <span>Decision message</span><code>{data.decisionMessageId || '—'}</code>
          <span>Account snapshot</span><code>{data.accountMessageId || '—'}</code>
          <span>Strategy intents</span><code>{data.signalsMessageId || '—'}</code>
        </div>
      </Panel>

      <Panel title="Safety Interpretation" right={<span className="warning">evidence only</span>}>
        <div className="detail-stack">
          <div className="callout callout--info"><strong>Observed does not mean “trading ready”.</strong><span>{snapshot ? 'This page shows the current canonical MOCK account, not an approved risk decision.' : 'This page shows persisted PortfolioRisk evidence for one decision cycle.'} It does not infer kill-switch state, venue health or an all-system safety verdict.</span></div>
          <div className="key-value-grid">
            <span>Persisted policy settings</span><strong>{data.activeLimitsAvailable ? 'Available below' : 'Unavailable'}</strong>
            <span>Strategy constraint evaluation</span><strong>{data.riskEvaluations?.length ? 'Persisted below; whole-account safety is separate' : 'Not persisted'}</strong>
            <span>Current marked exposure</span><strong>{data.currentValuationAvailable ? 'Account snapshot weights below' : 'Not valued'}</strong>
            <span>Kill switch</span><strong>Not inferred</strong>
          </div>
        </div>
      </Panel>
    </section>

    {data.riskEvaluations && data.riskEvaluations.length > 0 && <>
      <p className="risk-evaluation-note">Configuration SHA-256: <code>{data.configurationFingerprint}</code>. Weights below are evaluated strategy weights before rebalance; HOLD preserves actual quantity and emits no order.</p>
      {data.riskEvaluations.map(evaluation => <Panel key={evaluation.strategyId} title={`Risk Evaluation · ${evaluation.strategyId} · ${evaluation.name}`} right={<span className="muted">{evaluation.state}</span>}>
        <div className="risk-evaluation-grid">
          <div><span>Sizer / strategy capital</span><strong>{evaluation.sizer} · {evaluation.capital}</strong></div>
          <div><span>Asset cap / gross cap</span><strong>{evaluation.assetLimit} / {evaluation.grossLimit}</strong></div>
          <div><span>Gross after asset caps / gross scaling</span><strong>{evaluation.grossAfterAssetCap} / {evaluation.grossScale}</strong></div>
          <div><span>Volatility evidence</span><strong>{evaluation.volatilityState}</strong></div>
          <div><span>Annualized target / raw signal volatility</span><strong>{evaluation.volatilityTarget} / {evaluation.rawSignalVolatility}</strong></div>
          <div><span>Volatility scaling</span><strong>{evaluation.volatilityScale}</strong></div>
          <div><span>Annualized volatility before / after constraints</span><strong>{evaluation.preConstraintVolatility} / {evaluation.postConstraintVolatility}</strong></div>
        </div>
        <div className="table-wrap"><table className="risk-evaluation-table"><thead><tr><th>Asset</th><th>Sized weight</th><th>After asset cap</th><th>After gross cap</th><th>Held quantity</th><th>Rebalance action</th><th>Reduction / policy reason</th></tr></thead><tbody>
          {evaluation.assets.length === 0 && <tr><td colSpan={7} className="muted">{evaluation.state === 'EVALUATED' ? 'No asset exposure requested in this evaluation.' : 'Sizing unavailable; existing holdings are preserved and caps were not evaluated.'}</td></tr>}
          {evaluation.assets.map(asset => <tr key={asset.asset}><td>{asset.asset}</td><td>{asset.sizedWeight}</td><td>{asset.assetCappedWeight}</td><td>{asset.approvedWeight}</td><td>{asset.currentQuantity}</td><td>{asset.action}</td><td>{asset.reduction}</td></tr>)}
        </tbody></table></div>
      </Panel>)}
    </>}

    {data.policies && data.policies.length > 0 && <Panel title="Persisted Strategy Policy" right={<span className="muted">Settings, not a breach evaluation</span>}><div className="table-wrap"><table><thead><tr><th>Strategy</th><th>Allocation</th><th>Sizing</th><th>Max gross</th><th>Max asset</th><th>Rebalance</th></tr></thead><tbody>{data.policies.map(policy => <tr key={policy.strategyId}><td>{policy.strategyId} · {policy.name}</td><td>{policy.allocationLabel}</td><td>{policy.sizer}</td><td>{policy.grossLimitLabel}</td><td>{policy.assetLimitLabel}</td><td>{policy.rebalance}</td></tr>)}</tbody></table></div><p className="muted">Limits apply to each strategy's capital, not automatically to the whole account. Binding rules and intermediate sizing must come from the runtime.</p></Panel>}
    <Panel title={snapshot ? 'Observed Marked Positions' : 'Approved Asset Targets / HOLD Evidence'} right={<span className="muted">{snapshot ? 'Account snapshot' : 'DecisionBatch · approved economic boundary'}</span>}>
      <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Signal</th><th>Approved Weight</th><th>Target Notional</th><th>Strategy IDs</th><th>Current Weight</th><th>Policy Limit</th></tr></thead><tbody>
        {assets.length === 0 && <tr><td colSpan={7} className="muted">{snapshot ? 'The replay account has no non-zero positions; strategy risk diagnostics remain unavailable.' : 'No asset-level signals, targets or positions were persisted in this decision. This does not establish zero risk.'}</td></tr>}
        {assets.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td className={signalClass(row.signal)}>{row.signal}</td><td>{row.approvedWeightLabel}</td><td>{row.targetNotionalLabel}</td><td>{row.strategyLabel}</td><td className="muted">{row.currentWeightLabel}</td><td className="muted">{row.limitLabel}</td></tr>)}
      </tbody></table></div>
    </Panel>

    <section className="risk-grid risk-grid--bottom">
      <Panel title="Risk Diagnostics Availability" right={<span className="warning">partial contract</span>}>
        <div className="breach-list">
          {missing.map(item => <div className="breach-row" key={item}><AlertTriangle size={14}/><div><strong>{snapshot ? 'Not exposed in snapshot' : 'Not persisted in decision'}</strong><span>{item}</span><small>The dashboard does not recompute this and present it as canonical truth.</small></div></div>)}
        </div>
      </Panel>
      <Panel title="Whole-Account / Venue Breaches" right={<span className="muted">not inferred</span>}>
        <div className="empty-state">{data.riskEvaluations?.length ? 'Strategy target cap reductions are reported above. Held account exposure and venue margin/liquidation breaches are not evaluated by this report.' : snapshot ? 'The replay snapshot does not expose an evaluated breach/warning report.' : 'This older PortfolioRisk checkpoint has no constraint evaluation report.'} Missing evidence does not mean there are no breaches.</div>
      </Panel>
    </section>
  </>
}

function MockRiskView({ data }: { data: RiskData }) {
  return <>
    <div className="page-heading">
      <div><h1>Risk & Safety</h1><p>Configured limits, current utilisation, breaches and safety state.</p></div>
      <div className="cycle-label"><ShieldCheck size={14}/><span>{data.riskState}</span></div>
    </div>

    <section className="risk-summary">
      {data.portfolioLimits.slice(0, 4).map(limit => <div className="mini-metric" key={limit.id}><span>{limit.label}</span><strong>{limit.currentLabel}</strong><small>Limit {limit.limitLabel}</small><Utilization value={limit.utilizationPct} state={limit.state}/></div>)}
    </section>

    <section className="risk-grid">
      <Panel title="Portfolio Limits" right={<span className="muted">Current vs configured</span>}>
        <div className="risk-limit-list">{data.portfolioLimits.map(limit => <div className="risk-limit" key={limit.id}><div><strong>{limit.label}</strong><span>{limit.currentLabel} / {limit.limitLabel}</span></div><Utilization value={limit.utilizationPct} state={limit.state}/><RiskState state={limit.state}/></div>)}</div>
      </Panel>

      <Panel title="Safety State" right={<span className="positive"><ShieldCheck size={12}/> protected</span>}>
        <div className="safety-card">
          <div className="safety-card__hero"><ShieldCheck size={28}/><div><strong>{data.safety.tradingState}</strong><span>{data.safety.reason}</span></div></div>
          <div className="key-value-grid"><span>Kill switch</span><strong>{data.safety.killSwitch}</strong><span>Last risk check</span><strong>{data.safety.lastRiskCheck}</strong><span>Risk revision</span><code>{data.safety.riskRevision}</code></div>
          <div className="safety-actions"><button disabled><Pause size={13}/> Pause</button><button disabled><Play size={13}/> Resume</button><button className="danger" disabled><Skull size={13}/> Kill switch</button></div>
          <div className="preview-note"><LockKeyhole size={13}/><span>Controls are visual only in Step 05. No command is sent to the trading system.</span></div>
        </div>
      </Panel>
    </section>

    <Panel title="Asset Limits" right={<span className="muted">Approved weights after risk</span>}>
      <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Current Weight</th><th>Approved Weight</th><th>Max Weight</th><th>Utilisation</th><th>State</th></tr></thead><tbody>{data.assetLimits.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td>{row.currentWeightLabel}</td><td>{row.approvedWeightLabel}</td><td>{row.maxWeightLabel}</td><td><Utilization value={row.utilizationPct} state={row.state}/></td><td><RiskState state={row.state}/></td></tr>)}</tbody></table></div>
    </Panel>

    <section className="risk-grid risk-grid--bottom">
      <Panel title="Exchange Allocation" right={<span className="muted">Capital availability</span>}>
        <div className="allocation-grid"><div><span>Assigned</span><strong>{data.exchangeAllocation.assignedLabel}</strong></div><div><span>Used</span><strong>{data.exchangeAllocation.usedLabel}</strong></div><div><span>Free</span><strong className="positive">{data.exchangeAllocation.freeLabel}</strong></div><div><span>Locked by orders</span><strong className={data.exchangeAllocation.lockedPct > 5 ? 'warning' : ''}>{data.exchangeAllocation.lockedLabel}</strong></div></div>
      </Panel>
      <Panel title="Breaches / Warnings" right={data.breaches.length ? <span className="warning"><AlertTriangle size={12}/> {data.breaches.length}</span> : <span className="positive">None</span>}>
        {data.breaches.length ? <div className="breach-list">{data.breaches.map(breach => <div className="breach-row" key={breach.rule}><ShieldAlert size={14}/><div><strong>{breach.rule}</strong><span>{breach.currentLabel} vs {breach.thresholdLabel}</span><small>{breach.action}</small></div><RiskState state={breach.state}/></div>)}</div> : <div className="empty-state">No active risk breaches.</div>}
      </Panel>
    </section>
  </>
}

export function RiskPage() {
  const { data, error, retry } = useDashboardResource('getRisk')

  if (error) return <DataLoadError title="Risk data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading risk…</div>

  return data.sourceMode === 'REAL' || data.evidenceKind ? <RealRiskView data={data}/> : <MockRiskView data={data}/>
}
