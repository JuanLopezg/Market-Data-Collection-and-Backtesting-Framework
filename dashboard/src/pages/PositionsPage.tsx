import { useMemo, useState } from 'react'
import { Download, Search } from 'lucide-react'
import { DataLoadError } from '../components/DataLoadError'
import { displayAmount, usd } from '../components/LineChart'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { ClosedPosition, Position } from '../types/dashboard'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'

// Positions: account figures, open positions and closed positions (flat to flat).
// Current W. = position value / equity; Target W. = latest approved risk target / equity;
// Δ Weight = what the next rebalance would still change. Fees and funding belong to the
// position they were paid on; Net PnL = PnL - fees + funding (funding + = received).
// Kraken LIVE marks are Kraken's current mark prices; Funding Rate is the market's current
// rate (+ = longs pay shorts), red when this position pays it.

const valued = (position: Position) => position.valuationAvailable !== false && Number.isFinite(position.currentPrice) && position.currentPrice > 0
const targeted = (position: Position) => valued(position) && position.targetAvailable !== false
const pnlKnown = (position: Position) => position.pnlAvailable !== false && Number.isFinite(position.pnlUsd)

const percent = (value: number) => `${value.toFixed(1)}%`
const signedPercent = (value: number) => `${value > 0 ? '+' : ''}${value.toFixed(1)}%`
const signedUsd = (value: number) => Math.abs(value) < 0.005 ? '0$' : `${value > 0 ? '+' : '-'}${usd(Math.abs(value))}`
// Prices keep six significant digits (small coins trade far below 1$).
const price = (value: number) => `${new Intl.NumberFormat('en-US', { maximumSignificantDigits: 6 }).format(value)}$`
const quantity = (value: number) => new Intl.NumberFormat('en-US', { maximumSignificantDigits: 8 }).format(value)
const notional = (position: Position) => valued(position) ? usd(position.quantity * position.currentPrice) : '—'
const tone = (value: number) => value > 0 ? 'positive' : value < 0 ? 'negative' : ''
// What the position receives at the current rate: a long receives a negative rate.
const fundingTone = (position: Position) => tone((position.side === 'Short' ? 1 : -1) * (position.fundingRateAnnualPct ?? 0))
const fundingRate = (position: Position) => position.fundingRateAvailable
  ? <span title={`${(position.fundingRateHourlyPct ?? 0).toFixed(5)}% per hour; + = longs pay shorts`}>{signedPercent(position.fundingRateAnnualPct ?? 0)}/y</span>
  : '—'
const PAGE = 20

function StopLoss({ position }: { position: Position }) {
  const stop = position.stopLoss
  if (!stop) return <>—</>
  const detail = `${quantity(stop.quantity)} units; ${stop.status}; moves to 70% of each daily close${stop.replacementInProgress ? '; replacement in progress' : ''}`
  return <span title={detail}>{price(stop.price)}{stop.distancePct !== undefined && ` (${signedPercent(stop.distancePct)})`}{stop.replacementInProgress && <small> · replacing</small>}</span>
}

function PositionRow({ position, showFunding }: { position: Position; showFunding: boolean }) {
  const delta = targeted(position) ? position.targetWeightPct - position.currentWeightPct : null
  const net = position.netPnlUsd
  const netKnown = pnlKnown(position) && position.feesAvailable && net !== undefined
  return <tr>
    <td><strong>{position.asset}</strong></td>
    <td className={position.side === 'Short' ? 'negative' : 'positive'}>{position.side}</td>
    <td>{position.openedAt || '—'}</td>
    <td>{position.quantityLabel}</td>
    <td>{notional(position)}</td>
    <td>{valued(position) && position.entryPrice > 0 ? price(position.entryPrice) : '—'}</td>
    <td>{valued(position) ? price(position.currentPrice) : '—'}</td>
    <td><StopLoss position={position}/></td>
    <td>{valued(position) ? percent(position.currentWeightPct) : '—'}</td>
    <td>{targeted(position) ? percent(position.targetWeightPct) : '—'}</td>
    <td className={delta === null || Math.abs(delta) < 0.05 ? 'muted' : delta > 0 ? 'positive' : 'negative'}>{delta === null ? '—' : signedPercent(delta)}</td>
    <td className={!pnlKnown(position) ? 'muted' : tone(position.pnlUsd)}>{pnlKnown(position) ? signedUsd(position.pnlUsd) : '—'}</td>
    <td>{position.feesAvailable ? usd(position.feesUsd ?? 0) : '—'}</td>
    {showFunding && <td className={fundingTone(position)}>{fundingRate(position)}</td>}
    {showFunding && <td className={tone(position.fundingUsd ?? 0)}>{position.fundingAvailable ? signedUsd(position.fundingUsd ?? 0) : '—'}</td>}
    <td className={netKnown ? tone(net!) : 'muted'}>{netKnown ? signedUsd(net!) : '—'}</td>
    <td><StatePill state={position.status}/></td>
  </tr>
}

function ClosedRow({ position, showFunding }: { position: ClosedPosition; showFunding: boolean }) {
  return <tr>
    <td><strong>{position.asset}</strong></td>
    <td className={position.side === 'Short' ? 'negative' : 'positive'}>{position.side}</td>
    <td>{position.openedAt}</td>
    <td>{position.closedAt}</td>
    <td>{quantity(position.quantity)}</td>
    <td>{price(position.entryPrice)}</td>
    <td>{price(position.exitPrice)}</td>
    <td className={tone(position.grossPnl)}>{signedUsd(position.grossPnl)}</td>
    <td>{usd(position.fees)}</td>
    {showFunding && <td className={tone(position.funding)}>{signedUsd(position.funding)}</td>}
    <td className={tone(position.netPnl)}><strong>{signedUsd(position.netPnl)}</strong></td>
    <td className={tone(position.netPnl)}>{signedPercent(position.netPnlPct)}</td>
  </tr>
}

export function PositionsPage() {
  const { data, error, retry } = useDashboardResource('getPositions')
  const [query, setQuery] = useState('')
  const [closedQuery, setClosedQuery] = useState('')
  const [closedLimit, setClosedLimit] = useState(PAGE)

  const filtered = useMemo(() => data?.positions.filter(position => position.asset.toLowerCase().includes(query.trim().toLowerCase())) ?? [], [data, query])
  const closed = useMemo(() => (data?.closedPositions ?? []).filter(position => position.asset.toLowerCase().includes(closedQuery.trim().toLowerCase())), [data, closedQuery])
  if (error) return <DataLoadError title="Positions unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading positions…</div>

  const showFunding = data.fundingAvailable === true
  const allClosed = data.closedPositions ?? []
  const realized = allClosed.reduce((sum, position) => sum + position.netPnl, 0)
  const fees = allClosed.reduce((sum, position) => sum + position.fees, 0) + data.positions.reduce((sum, position) => sum + (position.feesUsd ?? 0), 0)
  const funding = allClosed.reduce((sum, position) => sum + position.funding, 0) + data.positions.reduce((sum, position) => sum + (position.fundingUsd ?? 0), 0)
  const stats = [
    { label: 'Active Positions', value: String(data.activePositions), tone: '' },
    { label: 'Gross Exposure', value: displayAmount(data.grossExposure), tone: '' },
    { label: 'Net Exposure', value: displayAmount(data.netExposure), tone: '' },
    { label: 'Unrealized PnL', value: displayAmount(data.unrealizedPnl), tone: /\d/.test(data.unrealizedPnl) ? (data.unrealizedPnl.startsWith('-') ? 'negative' : 'positive') : '' },
    ...(data.closedAvailable ? [
      { label: 'Realized PnL (net)', value: signedUsd(realized), tone: tone(realized) },
      { label: 'Total Fees', value: usd(fees), tone: '' },
      ...(showFunding ? [{ label: 'Total Funding', value: signedUsd(funding), tone: tone(funding) }] : []),
    ] : []),
  ]

  return <>
    <div className="page-heading"><h1>Positions</h1></div>

    <section className="overview-stats">
      {stats.map(stat => <div className="overview-stat" key={stat.label}><span>{stat.label}</span><strong className={stat.tone}>{stat.value}</strong></div>)}
    </section>

    <section className="positions-layout">
      <Panel title={data.priceSource ? `Active Positions · marks: ${data.priceSource}` : 'Active Positions'} right={<label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Filter asset" /></label>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr>
          <th>Asset</th><th>Side</th><th>Opened</th><th>Qty</th><th>Notional</th><th>Entry</th><th>Mark</th><th>Stop-Loss</th><th>Current W.</th><th>Target W.</th><th>Δ Weight</th><th>PnL</th><th>Fees</th>{showFunding && <th>Funding Rate</th>}{showFunding && <th>Funding</th>}<th>Net PnL</th><th>Status</th>
        </tr></thead><tbody>
          {filtered.length === 0 && <tr><td colSpan={showFunding ? 17 : 15} className="empty-cell">{data.positions.length ? 'No positions match the filter' : 'No open positions'}</td></tr>}
          {filtered.map(position => <PositionRow key={position.asset} position={position} showFunding={showFunding}/>)}
        </tbody></table></div>
      </Panel>

      <Panel title="Closed Positions" right={<div className="panel-actions">
        <label className="search-box"><Search size={14}/><input value={closedQuery} onChange={event => setClosedQuery(event.target.value)} placeholder="Filter asset" /></label>
        {data.closedAvailable && <a className="button-secondary" href="/api/reports/closed-positions.csv" download><Download size={13}/> CSV</a>}
      </div>}>
        {!data.closedAvailable
          ? <div className="empty-cell">Trade history unavailable</div>
          : <>
            <div className="table-wrap"><table className="wide-table"><thead><tr>
              <th>Asset</th><th>Side</th><th>Opened</th><th>Closed</th><th>Qty</th><th>Entry</th><th>Exit</th><th>Gross PnL</th><th>Fees</th>{showFunding && <th>Funding</th>}<th>Net PnL</th><th>Net %</th>
            </tr></thead><tbody>
              {closed.length === 0 && <tr><td colSpan={showFunding ? 12 : 11} className="empty-cell">{allClosed.length ? 'No positions match the filter' : 'No closed positions yet'}</td></tr>}
              {closed.slice(0, closedLimit).map((position, index) => <ClosedRow key={`${position.asset}-${position.closedAt}-${index}`} position={position} showFunding={showFunding}/>)}
            </tbody></table></div>
            {closed.length > closedLimit && <div className="table-more"><span className="muted">{closedLimit} of {closed.length}</span><button className="ghost-button" onClick={() => setClosedLimit(limit => limit + PAGE)}>Show more</button></div>}
          </>}
      </Panel>
    </section>
  </>
}
