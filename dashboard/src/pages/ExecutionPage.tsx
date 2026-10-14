import { AlertTriangle, Search } from 'lucide-react'
import { useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { LineChart, displayAmount, usd } from '../components/LineChart'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { ExecutionData, ExecutionOrderState, LedgerEntry } from '../types/dashboard'

// Execution: open/filled/rejected orders and fees, daily slippage, the orders and the fills.
// Rejects and cancels/replacements only appear when there are any. Rows are not clickable.
// SKIPPED orders are ones the venue cannot take (asset not listed or not trading, size below
// its minimum); they are shown with the gateway's reason and are not counted as rejects.

const PAGE = 20   // rows shown before "Show more"

function OrderState({ state }: { state: ExecutionOrderState }) {
  const tone = state === 'FILLED' ? 'good' : state === 'REJECTED' ? 'bad' : state === 'NEW' || state === 'PARTIAL' || state === 'PENDING_CANCEL' ? 'warn' : 'muted'
  return <span className={`order-state order-state--${tone}`}>{state}</span>
}

function Side({ side }: { side: 'BUY' | 'SELL' }) {
  return <span className={side === 'BUY' ? 'positive' : 'negative'}>{side}</span>
}

const fmtQty = (value: number) => value.toLocaleString('en-US', { maximumFractionDigits: 6 })
const price = (value: number) => value > 0 ? `${new Intl.NumberFormat('en-US', { maximumSignificantDigits: 6 }).format(value)}$` : '—'
const bps = (value: number) => Math.abs(value) < 0.005 ? '0.00 bps' : `${value > 0 ? '+' : ''}${value.toFixed(2)} bps`

// Replay business time: YYYYMMDD plus a phase digit (1 = open, 9 = close). Other values pass through.
function businessTime(value: string) {
  const match = /^(\d{4})(\d{2})(\d{2})(\d)?$/.exec(value)
  if (!match) return value
  const phase = match[4] === '1' ? ' open' : match[4] === '9' ? ' close' : ''
  return `${match[1]}-${match[2]}-${match[3]}${phase}`
}

function ShowMore({ shown, total, onMore }: { shown: number; total: number; onMore: () => void }) {
  if (shown >= total) return null
  return <div className="table-more"><span className="muted">{shown} of {total}</span><button className="ghost-button" onClick={onMore}>Show more</button></div>
}

function OrdersPanel({ data }: { data: ExecutionData }) {
  const [query, setQuery] = useState('')
  const [stateFilter, setStateFilter] = useState<'ALL' | ExecutionOrderState>('ALL')
  const [limit, setLimit] = useState(PAGE)
  const orders = useMemo(() => {
    const q = query.trim().toLowerCase()
    return data.orders.filter(order => (!q || `${order.orderId} ${order.asset}`.toLowerCase().includes(q)) && (stateFilter === 'ALL' || order.state === stateFilter))
  }, [data, query, stateFilter])

  return <Panel title="Orders" right={<div className="table-actions">
    <label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Order ID or asset"/></label>
    <select className="compact-select" value={stateFilter} onChange={event => setStateFilter(event.target.value as 'ALL' | ExecutionOrderState)}><option>ALL</option><option>NEW</option><option>PARTIAL</option><option>FILLED</option><option>PENDING_CANCEL</option><option>CANCELED</option><option>REJECTED</option><option>SKIPPED</option></select>
  </div>}>
    <div className="table-wrap"><table className="wide-table execution-table"><thead><tr>
      <th>Order ID</th><th>Asset</th><th>Side</th><th>Qty</th><th>Filled</th><th>State</th><th>Expected</th><th>Avg Fill</th><th>Slippage</th><th>Fees</th><th>Submitted</th>
    </tr></thead><tbody>
      {orders.length === 0 && <tr><td colSpan={11} className="empty-cell">{data.orders.length ? 'No orders match the filters' : 'No orders yet'}</td></tr>}
      {orders.slice(0, limit).map(order => <tr key={order.orderId}>
        <td className="mono"><strong>{order.orderId}</strong></td>
        <td><strong>{order.asset}</strong></td>
        <td><Side side={order.side}/></td>
        <td>{fmtQty(order.quantity)}</td>
        <td>{fmtQty(order.filledQty)}</td>
        <td><OrderState state={order.state}/></td>
        <td>{displayAmount(order.expectedPriceLabel)}</td>
        <td>{displayAmount(order.avgFillPriceLabel)}</td>
        <td className={order.slippageBps > 2 ? 'warning' : order.slippageBps < 0 ? 'positive' : ''}>{order.slippageBpsLabel}</td>
        <td>{displayAmount(order.feesLabel)}</td>
        <td>{businessTime(order.submittedAt)}</td>
      </tr>)}
    </tbody></table></div>
    <ShowMore shown={Math.min(limit, orders.length)} total={orders.length} onMore={() => setLimit(limit + PAGE)}/>
  </Panel>
}

// One row per fill: when, which order, what and at what price.
function FillsPanel() {
  const { data: ledger, error, retry } = useDashboardResource('getLedger')
  const [query, setQuery] = useState('')
  const [limit, setLimit] = useState(PAGE)
  const fills = useMemo(() => {
    const q = query.trim().toLowerCase()
    return (ledger?.entries ?? []).filter((entry: LedgerEntry) => !q || `${entry.orderId} ${entry.asset}`.toLowerCase().includes(q))
  }, [ledger, query])

  const status = ledger?.validated ? <span className="positive">VALIDATED</span>
    : error || ledger?.error ? <span className="negative">BLOCKED</span> : <span className="muted">{ledger?.status ?? 'LOADING'}</span>
  return <Panel title="Fills" right={<div className="table-actions">{status}
    <label className="search-box"><Search size={14}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Order ID or asset"/></label>
  </div>}>
    {error && <div className="empty-cell">Fills unavailable. <button className="link-button" onClick={retry}>Retry</button></div>}
    {ledger?.error && <div className="negative ledger-blocker"><strong>Ledger blocked:</strong> {ledger.error}</div>}
    {ledger && <>
      <div className="table-wrap"><table className="wide-table"><thead><tr>
        <th>Time</th><th>Order ID</th><th>Asset</th><th>Side</th><th>Qty</th><th>Price</th><th>Notional</th><th>Fee</th>
      </tr></thead><tbody>
        {fills.length === 0 && <tr><td colSpan={8} className="empty-cell">{ledger.entries.length ? 'No fills match the filter' : 'No fills yet'}</td></tr>}
        {fills.slice(0, limit).map(entry => <tr key={entry.entryId}>
          <td>{businessTime(entry.timestamp)}</td>
          <td className="mono">{entry.orderId}</td>
          <td><strong>{entry.asset}</strong></td>
          <td><Side side={entry.side}/></td>
          <td>{fmtQty(entry.quantity)}</td>
          <td>{price(entry.price)}</td>
          <td>{usd(entry.price > 0 ? entry.quantity * entry.price : entry.grossNotional)}</td>
          <td>{usd(entry.commission)}</td>
        </tr>)}
      </tbody></table></div>
      <ShowMore shown={Math.min(limit, fills.length)} total={fills.length} onMore={() => setLimit(limit + PAGE)}/>
    </>}
  </Panel>
}

export function ExecutionPage() {
  const { data, error, retry } = useDashboardResource('getExecution')

  if (error) return <DataLoadError title="Execution unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading execution…</div>

  const stats = [
    { label: 'Open Orders', value: String(data.openOrders), tone: data.openOrders ? 'warning' : '' },
    { label: 'Filled Orders', value: String(data.filledOrders), tone: '' },
    { label: 'Rejected', value: String(data.rejectCount), tone: data.rejectCount ? 'negative' : '' },
    { label: 'Skipped by venue', value: String(data.skippedCount ?? 0), tone: data.skippedCount ? 'warning' : '' },
    { label: 'Total Fees', value: displayAmount(data.totalFeesLabel), tone: '' },
  ]
  const slippage = data.slippageHistory ?? []

  return <>
    <div className="page-heading"><h1>Execution</h1></div>

    <section className="overview-stats">
      {stats.map(stat => <div className="overview-stat" key={stat.label}><span>{stat.label}</span><strong className={stat.tone}>{stat.value}</strong></div>)}
    </section>

    <div className="execution-stack">
    <Panel title="Average Slippage" right={data.slippageAvailable ? <span className="muted">{data.avgSlippageBpsLabel} overall</span> : undefined}>
      {slippage.length > 0
        ? <LineChart data={slippage} lines={[{ key: 'bps', label: 'Slippage', color: 'var(--series-2)' }]} formatValue={bps} compact/>
        : <div className="overview-unavailable overview-unavailable--compact"><strong>No slippage data</strong></div>}
    </Panel>

    <OrdersPanel data={data}/>
    <FillsPanel/>

    {data.rejects.length > 0 && <Panel title="Skipped and rejected orders" right={<span className="warning"><AlertTriangle size={12}/> {data.rejects.length}</span>}>
      <div className="reject-list">{data.rejects.map(reject => <div className={`reject-card${reject.kind === 'SKIPPED' ? ' reject-card--skipped' : ''}`} key={reject.orderId}>
        <div><strong>{reject.asset} · {reject.orderId}</strong><span>{reject.kind === 'SKIPPED' ? 'SKIPPED' : 'REJECTED'} · {reject.code}</span></div>
        <p>{reject.title && <span className="reject-card__title">{reject.title}: </span>}{reject.reason}</p>
        <small>{reject.remediation}</small>
      </div>)}</div>
    </Panel>}

    {data.replacements.length > 0 && <Panel title="Cancels / Replacements">
      <div className="table-wrap"><table><thead><tr><th>Asset</th><th>Original</th><th>Cancel requested</th><th>Canceled</th><th>Replacement</th><th>State</th></tr></thead><tbody>{data.replacements.map(row => <tr key={row.originalOrderId}><td><strong>{row.asset}</strong></td><td className="mono">{row.originalOrderId}</td><td>{row.cancelRequestedAt}</td><td>{row.canceledAt}</td><td className="mono">{row.replacementOrderId}</td><td><OrderState state={row.replacementState}/></td></tr>)}</tbody></table></div>
    </Panel>}
    </div>

  </>
}
