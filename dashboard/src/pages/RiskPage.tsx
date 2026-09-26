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

  return <>
    <div className="page-heading">
      <div><h1>Risk & Safety</h1><p>Canonical approved PortfolioRisk decision boundary and persisted input evidence.</p></div>
      <div className="cycle-label"><ShieldCheck size={14}/><span>{data.riskState}</span></div>
    </div>

    <div className="real-source-banner"><strong>REAL DATA</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Decision ${data.sourceUpdatedAt}.` : ''}</span></div>

    <section className="risk-summary">
      <div className="mini-metric"><span>Decision</span><strong>{data.decisionTimestamp ?? '—'}</strong><small>PortfolioRisk durable checkpoint</small></div>
      <div className="mini-metric"><span>Reference Capital</span><strong>{data.referenceCapitalLabel ?? '—'}</strong><small>{data.strategyCount ?? 0} strategy decision(s)</small></div>
      <div className="mini-metric"><span>Approved Gross Target</span><strong>{data.approvedGrossTargetLabel ?? '—'}</strong><small>{data.approvedTargetNotionalLabel ?? '—'} absolute target notional</small></div>
      <div className="mini-metric"><span>Approved Net Target</span><strong>{data.approvedNetTargetLabel ?? '—'}</strong><small>Signed approved notional / reference capital</small></div>
    </section>

    <section className="risk-grid">
      <Panel title="Approved Decision Boundary" right={<span className="positive"><ShieldCheck size={12}/> canonical checkpoint</span>}>
        <div className="key-value-grid">
          <span>Account cash input</span><strong>{data.accountCashLabel ?? '—'}</strong>
          <span>Approved target assets</span><strong>{data.activeTargetCount ?? 0}</strong>
          <span>Non-zero input signals</span><strong>{data.nonZeroSignalCount ?? 0}</strong>
          <span>Decision correlation</span><code>{data.decisionCorrelationId || '—'}</code>
          <span>Decision message</span><code>{data.decisionMessageId || '—'}</code>
          <span>Account snapshot</span><code>{data.accountMessageId || '—'}</code>
          <span>Strategy intents</span><code>{data.signalsMessageId || '—'}</code>
        </div>
      </Panel>

      <Panel title="Safety Interpretation" right={<span className="warning">evidence only</span>}>
        <div className="detail-stack">
          <div className="callout callout--info"><strong>Approved does not mean “trading ready”.</strong><span>This page proves the persisted PortfolioRisk output for one decision cycle. It does not infer kill-switch state, reconciliation readiness, venue health or an all-system safety verdict.</span></div>
          <div className="key-value-grid">
            <span>Active runtime limits</span><strong>Not wired</strong>
            <span>Durable breach report</span><strong>Not persisted</strong>
            <span>Current marked exposure</span><strong>Not valued</strong>
            <span>Kill switch</span><strong>Not inferred</strong>
          </div>
        </div>
      </Panel>
    </section>

    <Panel title="Approved Asset Targets" right={<span className="muted">DecisionBatch · approved economic boundary</span>}>
      <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Signal</th><th>Approved Weight</th><th>Target Notional</th><th>Strategy IDs</th><th>Current Weight</th><th>Policy Limit</th></tr></thead><tbody>
        {assets.length === 0 && <tr><td colSpan={7} className="muted">No asset targets are present in the latest durable DecisionBatch.</td></tr>}
        {assets.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td className={signalClass(row.signal)}>{row.signal}</td><td>{row.approvedWeightLabel}</td><td>{row.targetNotionalLabel}</td><td>{row.strategyLabel}</td><td className="muted">{row.currentWeightLabel}</td><td className="muted">{row.limitLabel}</td></tr>)}
      </tbody></table></div>
    </Panel>

    <section className="risk-grid risk-grid--bottom">
      <Panel title="Risk Diagnostics Availability" right={<span className="warning">partial contract</span>}>
        <div className="breach-list">
          {missing.map(item => <div className="breach-row" key={item}><AlertTriangle size={14}/><div><strong>Not persisted</strong><span>{item}</span><small>The dashboard does not recompute this and present it as canonical truth.</small></div></div>)}
        </div>
      </Panel>
      <Panel title="Breaches / Warnings" right={<span className="muted">not inferred</span>}>
        <div className="empty-state">PortfolioRisk's current durable decision checkpoint does not contain a first-class breach/warning report. An empty list would be ambiguous, so Step 21 does not claim “None”.</div>
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

  return data.sourceMode === 'REAL' ? <RealRiskView data={data}/> : <MockRiskView data={data}/>
}
