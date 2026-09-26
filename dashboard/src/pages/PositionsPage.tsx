import { useEffect, useMemo, useState } from 'react'
import { Search } from 'lucide-react'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { Position, PositionsData } from '../types/dashboard'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'

function valueOrDash(available: boolean | undefined, value: string) {
  return available === false ? '—' : value
}

function PositionRow({ position, onSelect }: { position: Position; onSelect: (position: Position) => void }) {
  const weightsAvailable = position.valuationAvailable !== false && position.targetAvailable !== false
  const targetDelta = weightsAvailable ? position.targetWeightPct - position.currentWeightPct : null
  const pnlAvailable = position.pnlAvailable !== false

  return <tr className="clickable-row" onClick={() => onSelect(position)}>
    <td><strong>{position.asset}</strong></td>
    <td className={position.side === 'Short' ? 'negative' : 'positive'}>{position.side}</td>
    <td>{position.valuationAvailable === false ? '—' : `${position.currentWeightPct.toFixed(1)}%`}</td>
    <td>{position.targetAvailable === false ? '—' : `${position.targetWeightPct.toFixed(1)}%`}</td>
    <td className={targetDelta === null || Math.abs(targetDelta) < 0.05 ? 'muted' : targetDelta > 0 ? 'positive' : 'negative'}>{targetDelta === null ? '—' : `${targetDelta > 0 ? '+' : ''}${targetDelta.toFixed(1)}%`}</td>
    <td>{position.quantityLabel}</td>
    <td>{valueOrDash(position.valuationAvailable, position.entryPriceLabel)}</td>
    <td>{valueOrDash(position.valuationAvailable, position.currentPriceLabel)}</td>
    <td className={!pnlAvailable ? 'muted' : position.pnlUsd >= 0 ? 'positive' : 'negative'}>{pnlAvailable ? position.pnlUsdLabel : '—'}</td>
    <td className={!pnlAvailable ? 'muted' : position.pnlPct >= 0 ? 'positive' : 'negative'}>{pnlAvailable ? position.pnlPctLabel : '—'}</td>
    <td><StatePill state={position.status}/></td>
  </tr>
}

function RealDetail({ position }: { position: Position }) {
  const breakdown = position.strategyBreakdown ?? []
  return <div className="detail-stack">
    <div className="detail-status"><div><span>Reconciliation</span><StatePill state={position.status}/></div><small>Exchange reconciliation is not yet a durable dashboard read model, so alignment is not inferred.</small></div>
    <div className="key-value-grid">
      <span>Physical account quantity</span><strong>{position.localQty}</strong>
      <span>Effective local quantity</span><strong>{position.effectiveQty}</strong>
      <span>Approved target quantity</span><strong>—</strong>
      <span>Exchange quantity</span><strong>—</strong>
      <span>Reconciliation delta</span><strong>—</strong>
      <span>Valuation</span><strong>Not wired</strong>
    </div>
    <div className="callout callout--info"><strong>Strategy virtual positions</strong><span>{breakdown.length === 0 ? 'No non-zero strategy virtual position is present for this asset.' : breakdown.map(item => `Strategy ${item.strategyId}: ${item.quantityLabel}`).join(' · ')}</span></div>
  </div>
}

function MockDetail({ position }: { position: Position }) {
  return <div className="detail-stack">
    <div className="detail-status"><div><span>Alignment</span><StatePill state={position.status}/></div><small>Data-source boundary active · execution detail can be backed by the Dashboard API later.</small></div>
    <div className="key-value-grid">
      <span>Target quantity</span><strong>{position.targetQty}</strong>
      <span>Effective quantity</span><strong>{position.effectiveQty}</strong>
      <span>Local executed</span><strong>{position.localQty}</strong>
      <span>Exchange quantity</span><strong>{position.exchangeQty}</strong>
      <span>Reconciliation delta</span><strong>{position.deltaQty}</strong>
      <span>Current weight</span><strong>{position.currentWeightPct.toFixed(1)}%</strong>
      <span>Approved target</span><strong>{position.targetWeightPct.toFixed(1)}%</strong>
    </div>
    <div className="callout callout--info"><strong>Why? trace arrives through the Pipeline page.</strong><span>The selected asset can be traced through market → signal → target → order as real lineage sources are enabled.</span></div>
  </div>
}

export function PositionsPage() {
  const { data, error, retry } = useDashboardResource('getPositions')
  const [query, setQuery] = useState('')
  const [selected, setSelected] = useState<Position | null>(null)

  useEffect(() => {
    if (!data) return
    if (selected) {
      const current = data.positions.find(position => position.asset === selected.asset)
      if (current) {
        if (current !== selected) setSelected(current)
        return
      }
    }
    setSelected(data.positions[0] ?? null)
  }, [data, selected])

  const filtered = useMemo(() => data?.positions.filter(position => position.asset.toLowerCase().includes(query.trim().toLowerCase())) ?? [], [data, query])
  if (error) return <DataLoadError title="Positions unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading positions…</div>

  const isReal = data.sourceMode === 'REAL'

  return <>
    <div className="page-heading"><div><h1>Positions</h1><p>Durable account positions and strategy virtual positions from ExecutionState.</p></div></div>

    {isReal && <div className="real-source-banner"><strong>REAL DATA</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Persisted ${data.sourceUpdatedAt}.` : ''}</span></div>}

    <section className="metric-row">
      <div className="mini-metric"><span>Active Positions</span><strong>{data.activePositions}</strong><small>{isReal ? 'Non-zero physical account positions' : 'Persistent strategy positions'}</small></div>
      {isReal
        ? <div className="mini-metric"><span>Account Cash</span><strong>{data.accountCash ?? '—'}</strong><small>Durable ExecutionState snapshot</small></div>
        : <div className="mini-metric"><span>Gross Exposure</span><strong>{data.grossExposure}</strong><small>{data.grossExposurePct} of equity</small></div>}
      {isReal
        ? <div className="mini-metric"><span>Portfolio Valuation</span><strong>{data.totalEquity}</strong><small>Canonical price adapter not enabled yet</small></div>
        : <div className="mini-metric"><span>Net Exposure</span><strong>{data.netExposure}</strong><small>Directional net notional</small></div>}
      <div className="mini-metric"><span>Unrealized PnL</span><strong className={isReal ? 'muted' : 'positive'}>{data.unrealizedPnl}</strong><small>{isReal ? 'No durable accounting/PnL model yet' : 'Across active positions'}</small></div>
    </section>

    <section className="positions-layout">
      <Panel title="Active Positions" right={<label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Filter asset" /></label>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Side</th><th>Current W.</th><th>Target W.</th><th>Δ Weight</th><th>Qty</th><th>Entry</th><th>Mark</th><th>PnL</th><th>PnL %</th><th>Status</th></tr></thead><tbody>{filtered.length === 0 && <tr><td colSpan={11} className="muted">{isReal ? 'No non-zero physical positions in the persisted runtime snapshot.' : 'No positions match the filter.'}</td></tr>}{filtered.map(position => <PositionRow key={position.asset} position={position} onSelect={setSelected}/>)}</tbody></table></div>
      </Panel>

      <Panel title={selected ? `${selected.asset} Position Detail` : 'Position Detail'}>
        {selected ? (isReal ? <RealDetail position={selected}/> : <MockDetail position={selected}/>) : <div className="empty-state">{isReal ? 'No active physical position to inspect.' : 'Select a position to inspect it.'}</div>}
      </Panel>
    </section>
  </>
}
