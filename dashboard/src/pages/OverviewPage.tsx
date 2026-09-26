import { Link } from 'react-router-dom'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { HealthState, OverviewData, PositionAlignmentState, ReconciliationState } from '../types/dashboard'
import { LineChart } from '../components/LineChart'
import { Panel } from '../components/Panel'
import { StatCard } from '../components/StatCard'
import { StatePill } from '../components/StatePill'

function readinessClass(state: HealthState) {
  if (state === 'READY') return 'good'
  if (state === 'PAUSED') return 'bad'
  return 'warn'
}

function reconciliationCopy(state: ReconciliationState) {
  if (state === 'CLEAN') return 'Current retained exchange evidence agrees with durable local state.'
  if (state === 'BLOCKED') return 'A current reconciliation mismatch requires operational attention.'
  if (state === 'DRIFT') return 'A non-blocking mismatch is present.'
  return 'Closed-loop proof is pending or exchange evidence is not current enough.'
}

function eventTone(severity: string) {
  if (severity === 'CRITICAL') return 'critical'
  if (severity === 'WARN') return 'warn'
  return 'info'
}

function pnlClass(available?: boolean, value?: number) {
  if (!available) return ''
  return (value ?? 0) < 0 ? 'negative' : 'positive'
}

export function OverviewPage() {
  const { data, error, retry } = useDashboardResource('getOverview')
  if (error) return <DataLoadError title="Overview unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading overview…</div>

  const isReal = data.sourceMode === 'REAL'
  const readiness: HealthState = data.readiness ?? 'READY'
  const reconciliation: ReconciliationState = data.reconciliationStatus ?? 'CLEAN'
  const equityAvailable = data.equityAvailable ?? data.equityCurve.length > 0
  const liveExpectedAvailable = data.liveExpectedAvailable ?? data.liveExpected.length > 0
  const checks = data.checks ?? [
    { label: 'Exchange vs Local', state: 'ALIGNED' as PositionAlignmentState, detail: 'Mock reconciliation state.' },
    { label: 'Positions', state: 'ALIGNED' as PositionAlignmentState, detail: 'Mock position state.' },
    { label: 'Balances', state: 'ALIGNED' as PositionAlignmentState, detail: 'Mock balance state.' },
  ]

  return <>
    <div className="page-heading">
      <div>
        <h1>Overview</h1>
        <p>{isReal ? 'Verified operational snapshot from canonical read-only sources.' : 'Portfolio performance, system health and trading activity at a glance.'}</p>
      </div>
      {isReal
        ? <div className="overview-cycle"><span>REAL DATA</span><strong>{data.canonicalCycle || 'Current snapshot'}</strong></div>
        : <div className="range"><button>1D</button><button className="active">7D</button><button>30D</button><button>90D</button><button>1Y</button></div>}
    </div>

    {isReal && <div className="real-source-banner">
      <strong>STEP 23 · REAL OVERVIEW</strong>
      <span>{data.sourceNote}</span>
      {data.sourceUpdatedAt && <small>Durable runtime persisted {data.sourceUpdatedAt}</small>}
    </div>}

    {data.sourceWarnings && data.sourceWarnings.length > 0 && <div className="callout callout--warn overview-warning-list">
      <strong>Partial source degradation</strong>
      {data.sourceWarnings.map((warning, index) => <span key={index}>{warning}</span>)}
    </div>}

    <section className="stats-grid">
      {data.stats.map(s => <StatCard key={s.label} metric={s}/>)}
      <div className={`stat-card readiness readiness--${readinessClass(readiness)}`}>
        <div>
          <div className="stat-card__label">System Readiness</div>
          <div className={`stat-card__value status-text status-text--${readinessClass(readiness)}`}>{readiness}</div>
          <div className="stat-card__sub">{data.readinessDetail ?? 'All systems operational'}</div>
        </div>
        <div className={`readiness-bars readiness-bars--${readinessClass(readiness)}`}>{Array.from({length:6}).map((_,i)=><span key={i}/>)}</div>
      </div>
    </section>

    <section className="grid-2-1">
      <Panel title="Equity Curve" right={<span className="muted">Canonical ledger required</span>}>
        {equityAvailable && data.equityCurve.length > 1
          ? <LineChart data={data.equityCurve}/>
          : <div className="overview-unavailable"><strong>Not available yet</strong><span>No canonical ledger / marked-equity history is wired, so Step 23 does not invent an equity curve.</span></div>}
      </Panel>
      <Panel title="Live vs Expected Behaviour" right={<Link className="text-link" to="/live-vs-expected">Details →</Link>}>
        {liveExpectedAvailable && data.liveExpected.length > 1
          ? <LineChart data={data.liveExpected} compact/>
          : <div className="overview-unavailable overview-unavailable--compact"><strong>Baseline not wired yet</strong><span>Historical baseline comparison is reserved for the Live vs Expected step.</span></div>}
      </Panel>
    </section>

    <section className="grid-bottom">
      <Panel title="Active Positions" right={<Link className="text-link" to="/positions">View all positions →</Link>}>
        <div className="table-wrap"><table><thead><tr><th>Asset</th><th>Side</th><th>Qty</th><th>Entry</th><th>Mark</th><th>PnL</th><th>Status</th></tr></thead><tbody>
          {data.positions.length === 0
            ? <tr><td colSpan={7} className="empty-cell">No non-zero physical positions in the durable runtime snapshot.</td></tr>
            : data.positions.map(p=><tr key={p.asset}><td><strong>{p.asset}</strong></td><td className={p.side==='Short'?'negative':'positive'}>{p.side}</td><td>{p.quantityLabel}</td><td>{p.entryPriceLabel}</td><td>{p.currentPriceLabel}</td><td className={pnlClass(p.pnlAvailable, p.pnlUsd)}>{p.pnlUsdLabel}</td><td><StatePill state={p.status}/></td></tr>)}
        </tbody></table></div>
      </Panel>
      <Panel title="Reconciliation / Closed Loop" right={<Link className="text-link" to="/reconciliation">Details →</Link>}>
        <div className="reconciliation">
          <div className={`overview-recon overview-recon--${reconciliation.toLowerCase()}`}><div><span>Closed Loop Status</span><strong>{reconciliation}</strong><small>{reconciliationCopy(reconciliation)}</small></div></div>
          {checks.map(check=><div className="recon-row overview-check" key={check.label}><div><span>{check.label}</span><small>{check.detail}</small></div><StatePill state={check.state}/></div>)}
          {isReal && <div className="info-box">Read-only evidence only.<small>The dashboard does not publish reconciliation or trading commands.</small></div>}
        </div>
      </Panel>
      <Panel title={isReal ? 'Recent Canonical Observations' : 'Recent Events'} right={<Link className="text-link" to="/alerts-audit">Alerts & Audit →</Link>}>
        <div className="events">{data.recentEvents.map((e,i)=><div className="event overview-event" key={`${e.time}-${i}`}><span className={`event-dot ${eventTone(e.severity)}`}/><time>{e.time}</time><span className="event__text">{e.event}</span><span className="event__source">{e.source}</span><strong className={e.severity==='CRITICAL'?'negative':e.severity==='WARN'?'warning':'info'}>{e.severity}</strong></div>)}</div>
      </Panel>
    </section>
  </>
}
