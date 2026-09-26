import { ChevronRight, CircleDot, Filter, Search, Waypoints } from 'lucide-react'
import { Fragment, useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'
import { WhyInspector } from '../components/WhyInspector'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { PipelineAssetRow, PipelineData, PipelineStageState } from '../types/dashboard'

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
      <b>{stage.value}</b>
      <small>{stage.detail}</small>
      {index < row.stages.length - 1 && <ChevronRight className="pipeline-stage__arrow" size={16}/>} 
    </div>)}
  </div>
}

export function PipelinePage() {
  const { data, error, retry } = useDashboardResource('getPipeline')
  const [selectedCycle, setSelectedCycle] = useState<string | null>(null)
  const [expandedCycle, setExpandedCycle] = useState<string | null>(null)
  const [query, setQuery] = useState('')


  const rows = useMemo(() => data?.rows.filter(row => row.asset.toLowerCase().includes(query.toLowerCase())) ?? [], [data, query])
  if (error) return <DataLoadError title="Pipeline unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading pipeline…</div>
  const selectedTrace = selectedCycle ? data.traces[selectedCycle] : null

  return <>
    <div className="page-heading">
      <div><h1>Strategy → Portfolio Pipeline</h1><p>Explain every transformation from market observation to exchange state.</p></div>
      <div className="cycle-label"><CircleDot size={14}/><span>{data.cycleLabel}</span></div>
    </div>

    {data.sourceMode === 'REAL' && <div className="real-source-banner"><strong>REAL DATA · cycle-aligned durable lineage</strong><span>{data.sourceNote}</span></div>}

    {data.proof && <Panel title="Step 31 · End-to-End Proof" right={<StatePill state={data.proof.status}/>}>
      <div className="e2e-proof-head">
        <div><span>Cycle</span><strong>{data.proof.cycleTimestamp}</strong><small>{data.proof.completion.replace(/_/g, ' ')}</small></div>
        <div><span>Correlation</span><strong>{data.proof.correlationId}</strong><small>Durable decision metadata</small></div>
        <div><span>Orders</span><strong>{data.proof.matchedOrders}/{data.proof.submitOrders}</strong><small>{data.proof.cancelOrders} cancel request(s)</small></div>
        <div><span>Fill coverage</span><strong>{data.proof.matchedFills}/{data.proof.submitOrders}</strong><small>{data.proof.fillIds.length} persisted fill row(s)</small></div>
      </div>
      <div className="e2e-proof-grid">
        {data.proof.steps.map((step, index) => <div className="e2e-proof-step" key={step.id}>
          <div className="e2e-proof-step__top"><span>{index + 1}</span><StatePill state={step.state}/></div>
          <strong>{step.label}</strong>
          <p>{step.evidence}</p>
          <small>{step.source}</small>
        </div>)}
      </div>
      <div className="e2e-proof-note">{data.proof.sourceNote}</div>
    </Panel>}

    <div className="pipeline-summary">
      <div className="mini-metric"><span>Latest decision</span><strong>{data.latestDecision}</strong><small>Authoritative business time</small></div>
      <div className="mini-metric"><span>Universe</span><strong>{data.universeSize}</strong><small>Top liquidity assets</small></div>
      <div className="mini-metric"><span>Active signals</span><strong>{data.activeSignals}</strong><small>Persistent strategy state</small></div>
      <div className="mini-metric"><span>Actionable plans</span><strong>{data.actionablePlans}</strong><small>Orders or closes required</small></div>
    </div>

    <Panel title="Decision Matrix" right={<div className="table-actions"><div className="search-box"><Search size={13}/><input value={query} onChange={e=>setQuery(e.target.value)} placeholder="Asset…"/></div><button className="ghost-button"><Filter size={13}/> Filters</button></div>}>
      <div className="table-wrap"><table className="wide-table pipeline-table"><thead><tr><th>Asset</th><th>Rank</th><th>RSI(7)</th><th>Signal</th><th>Raw target</th><th>Vol target</th><th>Approved</th><th>Current</th><th>Delta</th><th>Plan</th><th>Exchange</th><th></th></tr></thead>
      <tbody>{rows.map(row => <Fragment key={row.cycleId}>
        <tr key={row.cycleId} className="clickable-row" onClick={()=>setExpandedCycle(expandedCycle === row.cycleId ? null : row.cycleId)}>
          <td><strong>{row.asset}</strong><small className="cell-sub">{row.decisionTime}</small></td><td>{row.rankLabel ?? `#${row.rank}`}</td><td className={!row.rsiLabel && row.rsi > 80 ? 'warning' : ''}>{row.rsiLabel ?? row.rsi.toFixed(1)}</td><td><span className={`signal signal--${row.signal.toLowerCase()}`}>{row.signal}</span></td><td>{row.rawTargetLabel ?? `${row.rawTargetPct.toFixed(1)}%`}</td><td>{row.volTargetLabel ?? `${row.volTargetPct.toFixed(1)}%`}</td><td><strong>{row.approvedTargetLabel ?? `${row.approvedTargetPct.toFixed(1)}%`}</strong></td><td>{row.currentWeightLabel ?? `${row.currentWeightPct.toFixed(1)}%`}</td><td className={row.requiredDeltaLabel ? '' : row.requiredDeltaPct === 0 ? '' : row.requiredDeltaPct > 0 ? 'positive' : 'warning'}>{row.requiredDeltaLabel ?? `${row.requiredDeltaPct > 0 ? '+' : ''}${row.requiredDeltaPct.toFixed(1)}%`}</td><td>{row.plannedAction}</td><td><span className={`exchange-state exchange-state--${row.exchangeState.toLowerCase()}`}>{row.exchangeState}</span></td><td><button className="why-button" onClick={(e)=>{e.stopPropagation();setSelectedCycle(row.cycleId)}}><Waypoints size={13}/> Why?</button></td>
        </tr>
        {expandedCycle === row.cycleId && <tr key={`${row.cycleId}-rail`} className="pipeline-expanded"><td colSpan={12}><PipelineRail row={row}/></td></tr>}
      </Fragment>)}</tbody></table></div>
    </Panel>

    <div className="pipeline-help">
      <div><strong>Interaction model</strong><span>Click a row to expand the stage-by-stage transformation. Use “Why?” for the complete trading-cycle inspector.</span></div>
      <StatePill state="ALIGNED"/>
    </div>

    {selectedTrace && <><div className="inspector-backdrop" onClick={()=>setSelectedCycle(null)}/><WhyInspector trace={selectedTrace} onClose={()=>setSelectedCycle(null)}/></>}
  </>
}
