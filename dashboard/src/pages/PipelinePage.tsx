import { ChevronRight, Search, Waypoints } from 'lucide-react'
import { Fragment, useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'
import { WhyInspector } from '../components/WhyInspector'
import { DataLoadError } from '../components/DataLoadError'
import { displayAmount } from '../components/LineChart'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { EndToEndProof, PipelineAssetRow, PipelineData, PipelineStageState } from '../types/dashboard'

// Pipeline: the latest close decision asset by asset. The matrix shows the aggregated
// signal (-1..+1, strategies weighted by capital share) and what followed from it. Click a
// row for the per-strategy breakdown and the stage rail, "Trace" for the full cycle.

function StageState({ state }: { state: PipelineStageState }) {
  const cls = state === 'OK' ? 'stage-state stage-state--ok' : state === 'CHANGED' ? 'stage-state stage-state--changed' : state === 'BLOCKED' ? 'stage-state stage-state--blocked' : 'stage-state stage-state--pending'
  return <span className={cls}>{state}</span>
}

function PipelineRail({ row }: { row: PipelineAssetRow }) {
  return <div className="pipeline-rail">
    {row.stages.map((stage, index) => <div className="pipeline-stage" key={stage.id}>
      <div className="pipeline-stage__top">
        <span>{index + 1}</span>
        <StageState state={stage.state}/>
      </div>
      <strong>{stage.label}</strong>
      <b>{displayAmount(stage.value)}</b>
      <small>{stage.detail}</small>
      {index < row.stages.length - 1 && <ChevronRight className="pipeline-stage__arrow" size={16}/>}
    </div>)}
  </div>
}

const DECISION_TONE: Record<string, string> = { ENTER: 'positive', EXIT: 'negative', RESIZE: 'info', HOLD: 'muted' }

// Signal in [-1, +1]: +0.50 long, -0.50 short, 0.00 flat.
const signalText = (value: number) => Math.abs(value) < 0.005 ? '0.00' : `${value > 0 ? '+' : ''}${value.toFixed(2)}`
const signalTone = (value: number) => Math.abs(value) < 0.005 ? 'muted' : value > 0 ? 'positive' : 'negative'

// Rows without a numeric signal (older payloads) fall back to the LONG/SHORT/FLAT label.
function rowSignal(row: PipelineAssetRow) {
  if (row.signalValue !== undefined) return row.signalValue
  return row.signal === 'LONG' ? 1 : row.signal === 'SHORT' ? -1 : row.signal === 'FLAT' ? 0 : null
}

function SignalCell({ value }: { value: number | null }) {
  return <td className={value === null ? 'muted' : signalTone(value)}>{value === null ? '—' : signalText(value)}</td>
}

// Each strategy's signal and risk outcome, then the capital-weighted aggregate.
function StrategyBreakdown({ row }: { row: PipelineAssetRow }) {
  const strategies = row.strategies ?? []
  if (!strategies.length) return null
  const aggregated = rowSignal(row)
  return <div className="pipeline-breakdown">
    <table><thead><tr>
      <th>Strategy</th><th>Capital</th><th>Weight in signal</th><th>Signal</th><th>Indicator</th><th>Sized W.</th><th>Approved W.</th><th>Decision</th>
    </tr></thead><tbody>
      {strategies.map(strategy => <tr key={strategy.name}>
        <td><strong>{strategy.name}</strong>{strategy.rule && <small className="cell-sub">{strategy.rule}</small>}</td>
        <td>{strategy.capitalLabel}</td>
        <td>{strategy.shareLabel}</td>
        <td className={signalTone(strategy.signal)}>{strategy.signalLabel}</td>
        <td>{strategy.detail}</td>
        <td>{strategy.sizedLabel}</td>
        <td>{strategy.approvedLabel}</td>
        <td className={DECISION_TONE[strategy.decision] ?? 'muted'}>{strategy.decision}</td>
      </tr>)}
      <tr className="pipeline-breakdown__total">
        <td><strong>Aggregated</strong></td><td></td><td>100%</td>
        <td className={aggregated === null ? 'muted' : signalTone(aggregated)}>{aggregated === null ? '—' : signalText(aggregated)}</td>
        <td></td>
        <td>{row.rawTargetLabel ?? '—'}</td>
        <td>{row.approvedTargetLabel ?? '—'}</td>
        <td className={DECISION_TONE[row.decisionLabel ?? ''] ?? 'muted'}>{row.decisionLabel ?? '—'}</td>
      </tr>
    </tbody></table>
  </div>
}

// Changes first, then the universe by liquidity rank, then the rest alphabetically.
function orderRows(rows: PipelineAssetRow[]) {
  const changed = (row: PipelineAssetRow) => row.decisionLabel && row.decisionLabel !== 'HOLD' ? 0 : 1
  const rank = (row: PipelineAssetRow) => row.rank > 0 ? row.rank : Number.MAX_SAFE_INTEGER
  return [...rows].sort((a, b) => changed(a) - changed(b) || rank(a) - rank(b) || a.asset.localeCompare(b.asset))
}

// Last cycle check (real data only): did the latest close travel the whole chain, from
// market data to fills and reconciliation? One line of counts and one chip per stage; the
// evidence text is in each chip's tooltip and listed only for stages that are not aligned.
// The "Dashboard Projection" stage describes the dashboard itself and is left out.
const CYCLE_STAGE_LABEL: Record<string, string> = {
  'Risk / Approved Target': 'Risk',
  'Order Planning': 'Planning',
  'Execution Order': 'Orders',
  'Persisted Fill': 'Fills',
}

const STAGE_DOT: Record<string, string> = { ALIGNED: 'dot--good', BLOCKED: 'dot--bad' }

function cycleSummary(proof: EndToEndProof) {
  if (proof.submitOrders === 0) return proof.cancelOrders ? `No new orders · ${proof.cancelOrders} cancel request(s)` : 'No orders this cycle'
  const parts = [`${proof.submitOrders} order(s) planned`, `${proof.matchedOrders} at the exchange`, `${proof.matchedFills} filled`]
  if (proof.cancelOrders) parts.push(`${proof.cancelOrders} cancel request(s)`)
  return parts.join(' · ')
}

function CycleCheck({ proof }: { proof: EndToEndProof }) {
  const stages = proof.steps.filter(step => step.label !== 'Dashboard Projection')
  const open = stages.filter(step => step.state !== 'ALIGNED')
  return <Panel title={`Last Cycle Check · ${proof.cycleTimestamp}`} right={<StatePill state={proof.status}/>} className="pipeline-proof">
    <p className="cycle-check__summary">{cycleSummary(proof)}</p>
    <div className="cycle-check__chain">
      {stages.map((step, index) => <Fragment key={step.id}>
        {index > 0 && <ChevronRight size={14} className="muted"/>}
        <span className="cycle-check__stage" title={step.evidence}>
          <span className={`dot ${STAGE_DOT[step.state] ?? 'dot--warn'}`}/>{CYCLE_STAGE_LABEL[step.label] ?? step.label}
        </span>
      </Fragment>)}
    </div>
    {open.length > 0 && <ul className="cycle-check__open">
      {open.map(step => <li key={step.id}><strong>{CYCLE_STAGE_LABEL[step.label] ?? step.label}:</strong> {step.evidence}</li>)}
    </ul>}
  </Panel>
}

function summaryOf(data: PipelineData) {
  if (data.summary?.length) return data.summary
  return [
    { label: 'Decision', value: data.latestDecision },
    { label: 'Universe', value: String(data.universeSize) },
    { label: 'Active Signals', value: String(data.activeSignals) },
    { label: 'Orders', value: String(data.actionablePlans) },
  ]
}

export function PipelinePage() {
  const { data, error, retry } = useDashboardResource('getPipeline')
  const [selectedCycle, setSelectedCycle] = useState<string | null>(null)
  const [expandedCycle, setExpandedCycle] = useState<string | null>(null)
  const [query, setQuery] = useState('')

  const rows = useMemo(() => orderRows(data?.rows.filter(row => row.asset.toLowerCase().includes(query.trim().toLowerCase())) ?? []), [data, query])
  if (error) return <DataLoadError title="Pipeline unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading pipeline…</div>
  const selectedTrace = selectedCycle ? data.traces[selectedCycle] : null

  return <>
    <div className="page-heading"><h1>Pipeline</h1></div>

    <section className="overview-stats">
      {summaryOf(data).map(stat => <div className="overview-stat" key={stat.label}><span>{stat.label}</span><strong>{stat.value}</strong></div>)}
    </section>

    <Panel title="Decision Matrix" right={<label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Filter asset"/></label>}>
      <div className="table-wrap"><table className="wide-table pipeline-table"><thead><tr>
        <th>Asset</th><th>Liq. Rank</th><th>Signal</th><th>Sized W.</th><th>Approved W.</th><th>Decision</th><th>Target</th><th>Current</th><th>Delta</th><th>Order</th><th>Execution</th><th></th>
      </tr></thead>
      <tbody>
        {rows.length === 0 && <tr><td colSpan={12} className="empty-cell">{query ? 'No assets match the filter' : data.emptyReason ?? 'No decision yet'}</td></tr>}
        {rows.map(row => <Fragment key={row.cycleId}>
          <tr className={`clickable-row${row.inUniverse === false ? ' pipeline-row--outside' : ''}`} onClick={() => setExpandedCycle(expandedCycle === row.cycleId ? null : row.cycleId)}>
            <td><strong>{row.asset}</strong></td>
            <td>{row.rankLabel ?? `#${row.rank}`}</td>
            <SignalCell value={rowSignal(row)}/>
            <td>{row.rawTargetLabel ?? `${row.rawTargetPct.toFixed(1)}%`}</td>
            <td><strong>{row.approvedTargetLabel ?? `${row.approvedTargetPct.toFixed(1)}%`}</strong></td>
            <td className={DECISION_TONE[row.decisionLabel ?? ''] ?? 'muted'}>{row.decisionLabel ?? '—'}</td>
            <td>{displayAmount(row.targetLabel ?? '—')}</td>
            <td>{displayAmount(row.currentWeightLabel ?? '—')}</td>
            <td className={row.requiredDeltaLabel?.startsWith('+') ? 'positive' : row.requiredDeltaLabel?.startsWith('-') ? 'negative' : ''}>{displayAmount(row.requiredDeltaLabel ?? '—')}</td>
            <td>{row.plannedAction}</td>
            <td><span className={`exchange-state exchange-state--${row.exchangeState.toLowerCase()}`}>{row.exchangeState}</span></td>
            <td><button className="why-button" onClick={event => { event.stopPropagation(); setSelectedCycle(row.cycleId) }}><Waypoints size={13}/> Trace</button></td>
          </tr>
          {expandedCycle === row.cycleId && <tr className="pipeline-expanded"><td colSpan={12}><StrategyBreakdown row={row}/><PipelineRail row={row}/></td></tr>}
        </Fragment>)}
      </tbody></table></div>
    </Panel>

    {data.proof && <CycleCheck proof={data.proof}/>}

    {selectedTrace && <><div className="inspector-backdrop" onClick={() => setSelectedCycle(null)}/><WhyInspector trace={selectedTrace} onClose={() => setSelectedCycle(null)}/></>}
  </>
}
