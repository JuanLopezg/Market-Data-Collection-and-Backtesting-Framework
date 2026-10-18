import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { ReconciliationData, ReconciliationRow } from '../types/dashboard'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'

// Reconciliation: do our books (positions and open orders we recorded) match what the
// exchange reports? The runtime checks this before every new order and blocks new
// orders while they disagree. The dashboard only reads and compares.

const qty = (value: number) => Number.isInteger(value) ? value.toLocaleString('en-US') : value.toLocaleString('en-US', { maximumFractionDigits: 8 })

const STATUS_TEXT: Record<string, string> = {
  CLEAN: 'Our records match the exchange.',
  PENDING: 'Waiting for up-to-date exchange data; nothing is proven yet.',
  DRIFT: 'A small difference was found.',
  BLOCKED: 'Our records and the exchange disagree; new orders stay blocked until it is resolved.',
}

// Reconciler issue kinds in plain words.
const ISSUE_TEXT: Record<string, string> = {
  CashMismatch: 'Cash differs',
  PositionMismatch: 'Position differs',
  MissingExchangeOrder: 'Our open order is missing at the exchange',
  OrderMismatch: 'Order details differ',
  UnexpectedExchangeOrder: 'The exchange has an order we do not know',
}

function freshnessNote(data: ReconciliationData) {
  if (!data.comparisonAvailable) return 'No exchange data to compare with yet.'
  if (!data.evidenceFresh) return 'The latest exchange data is older than our records; waiting for a newer snapshot.'
  return ''
}

function PositionRow({ row }: { row: ReconciliationRow }) {
  const differs = row.exchangeAvailable !== false && row.deltaQty !== 0
  return <tr title={row.explanation}>
    <td><strong>{row.asset}</strong></td>
    <td>{row.localQtyLabel ?? qty(row.localQty)}</td>
    <td>{row.exchangeAvailable === false ? '—' : row.exchangeQtyLabel ?? qty(row.exchangeQty)}</td>
    <td className={differs ? 'negative' : 'muted'}>{row.exchangeAvailable === false ? '—' : row.deltaQtyLabel ?? qty(row.deltaQty)}</td>
    <td className={row.pendingQty === 0 ? 'muted' : ''}>{row.pendingQtyLabel ?? qty(row.pendingQty)}</td>
    <td><StatePill state={row.status}/></td>
  </tr>
}

export function ReconciliationPage() {
  const { data, error, retry } = useDashboardResource('getReconciliation')
  if (error) return <DataLoadError title="Reconciliation unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading reconciliation…</div>

  const issues = data.issues ?? []
  const compared = data.rows.filter(row => row.exchangeAvailable !== false)
  const differing = compared.filter(row => row.deltaQty !== 0).length
  const note = freshnessNote(data)
  const statusTone = data.status === 'CLEAN' ? 'positive' : data.status === 'BLOCKED' ? 'negative' : 'status-warn'
  const stats = [
    { label: 'Status', value: data.status, tone: statusTone, detail: STATUS_TEXT[data.status] ?? '' },
    { label: 'Last Checked', value: data.lastChecked || '—', tone: '', detail: '' },
    { label: 'Assets Compared', value: String(compared.length), tone: '', detail: differing ? `${differing} differ` : 'all match' },
    { label: 'Open Orders', value: String(data.openOrders.length), tone: '', detail: '' },
  ]

  return <>
    <div className="page-heading"><h1>Reconciliation</h1></div>

    <section className="overview-stats">
      {stats.map(stat => <div className="overview-stat" key={stat.label}>
        <span>{stat.label}</span><strong className={stat.tone}>{stat.value}</strong>
        {stat.detail && <small className="muted">{stat.detail}</small>}
      </div>)}
    </section>

    {note && <div className="callout callout--warn"><span>{note}</span></div>}

    <section className="positions-layout">
      <Panel title="Positions: Ours vs Exchange" right={<span className="muted">Tolerance: {data.tolerance}</span>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr>
          <th>Asset</th><th>Ours</th><th>Exchange</th><th>Difference</th><th>Open Orders</th><th>Status</th>
        </tr></thead><tbody>
          {data.rows.length === 0
            ? <tr><td colSpan={6} className="empty-cell">No positions or open orders to compare</td></tr>
            : data.rows.map(row => <PositionRow key={row.asset} row={row}/>)}
        </tbody></table></div>
      </Panel>

      {issues.length > 0 && <Panel title={`Problems Found (${issues.length})`}>
        <div className="table-wrap"><table><thead><tr><th>Problem</th><th>Asset</th><th>Order</th><th>Ours</th><th>Exchange</th><th>Detail</th></tr></thead><tbody>
          {issues.map((issue, index) => <tr key={`${issue.kind}-${issue.orderId}-${issue.asset}-${index}`}>
            <td><strong>{ISSUE_TEXT[issue.kind] ?? issue.kind}</strong></td>
            <td>{issue.asset || '—'}</td>
            <td className="mono">{issue.orderId || '—'}</td>
            <td>{qty(issue.localValue)}</td>
            <td>{qty(issue.exchangeValue)}</td>
            <td>{issue.message}</td>
          </tr>)}
        </tbody></table></div>
      </Panel>}

      <Panel title="Open Orders">
        <div className="table-wrap"><table><thead><tr><th>Order</th><th>Asset</th><th>Side</th><th>Qty</th><th>Remaining</th><th>Active From</th><th>State</th><th>Exchange ID</th></tr></thead><tbody>
          {data.openOrders.length === 0
            ? <tr><td colSpan={8} className="empty-cell">No open orders</td></tr>
            : data.openOrders.map(order => <tr key={order.orderId}>
              <td className="mono">{order.orderId}{order.orderType === 'STOP' && <span title={`Resting stop-loss at ${order.positionStopPrice}$; moves to 70% of each daily close`}> · STOP</span>}</td>
              <td><strong>{order.asset}</strong></td>
              <td className={order.side === 'BUY' ? 'positive' : 'negative'}>{order.side}</td>
              <td>{qty(order.quantity)}</td>
              <td>{qty(order.remaining)}</td>
              <td>{order.age}</td>
              <td>{order.state}</td>
              <td className="mono muted">{order.exchangeId}</td>
            </tr>)}
        </tbody></table></div>
      </Panel>
    </section>
  </>
}
