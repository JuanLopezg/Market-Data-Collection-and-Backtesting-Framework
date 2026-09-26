import { AlertTriangle, CheckCircle2, Clock3, DatabaseZap } from 'lucide-react'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { ReconciliationData, ReconciliationRow } from '../types/dashboard'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'

function qty(value: number) {
  return Number.isInteger(value) ? value.toLocaleString() : value.toLocaleString(undefined, { maximumFractionDigits: 10 })
}

function display(row: ReconciliationRow, key: 'target' | 'effective' | 'local' | 'exchange' | 'pending' | 'delta') {
  switch (key) {
    case 'target': return row.targetQtyLabel ?? qty(row.targetQty)
    case 'effective': return row.effectiveQtyLabel ?? qty(row.effectiveQty)
    case 'local': return row.localQtyLabel ?? qty(row.localQty)
    case 'exchange': return row.exchangeQtyLabel ?? qty(row.exchangeQty)
    case 'pending': return row.pendingQtyLabel ?? qty(row.pendingQty)
    case 'delta': return row.deltaQtyLabel ?? qty(row.deltaQty)
  }
}

export function ReconciliationPage() {
  const { data, error, retry } = useDashboardResource('getReconciliation')
  if (error) return <DataLoadError title="Reconciliation unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading reconciliation…</div>

  const real = data.sourceMode === 'REAL'
  const issues = data.issues ?? []
  const cleanIcon = data.status === 'CLEAN' ? <CheckCircle2/> : data.status === 'BLOCKED' ? <AlertTriangle/> : <Clock3/>

  return <>
    <div className="page-heading"><div><h1>Reconciliation</h1><p>Closed-loop proof between durable ExecutionState and retained exchange truth.</p></div><StatePill state={data.status}/></div>

    {real && <div className={`callout ${data.status === 'CLEAN' ? 'callout--good' : data.status === 'BLOCKED' ? 'callout--bad' : 'callout--warn'}`}>
      <strong>REAL DATA · {data.comparisonAvailable ? (data.evidenceFresh ? 'current retained exchange evidence' : 'stale retained exchange evidence') : 'exchange evidence unavailable'}</strong>
      <span>{data.sourceNote}</span>
    </div>}

    <section className="recon-summary">
      <div className={`recon-hero recon-hero--${data.status === 'CLEAN' ? 'good' : data.status === 'BLOCKED' ? 'bad' : 'warn'}`}>{cleanIcon}<div><span>Closed Loop Status</span><strong>{data.status}</strong><small>Evidence {data.lastChecked}</small></div></div>
      <div className="mini-metric"><span>{real ? 'Planner Target' : 'Target Portfolio'}</span><strong>{data.targetPortfolioValue}</strong><small>{real ? 'Target quantity/value not wired into Reconciliation yet' : 'Risk-approved target value'}</small></div>
      <div className="mini-metric"><span>{real ? 'Local Durable State' : 'Local State'}</span><strong>{data.localPortfolioValue}</strong><small>trading_runtime_state.snapshot</small></div>
      <div className="mini-metric"><span>{real ? 'Exchange Evidence' : 'Exchange State'}</span><strong>{data.exchangePortfolioValue}</strong><small>{real && data.exchangeEvidenceSequence ? `JetStream seq ${data.exchangeEvidenceSequence}` : 'Venue-observed value'}</small></div>
    </section>

    {real && <section className="recon-layout">
      <Panel title="Evidence Boundary" right={<StatePill state={data.evidenceFresh ? 'ALIGNED' : 'PENDING'}/>}>
        <div className="semantics-list">
          <div><DatabaseZap/><p><strong>Local persisted:</strong> {data.sourceUpdatedAt || '—'}</p></div>
          <div><Clock3/><p><strong>Exchange event stored:</strong> {data.exchangeEvidenceTime || 'not retained'}</p></div>
          <div><span className="mono muted">TS</span><p><strong>Exchange logical timestamp:</strong> {data.exchangeSnapshotTimestamp || '—'}</p></div>
        </div>
      </Panel>
      <Panel title="Runtime Tolerances">
        <div className="callout"><strong>Same defaults as the C++ Reconciler</strong><span>{data.tolerance}. The dashboard reads and compares only; it does not request a venue snapshot, submit orders, cancel orders or change trading state.</span></div>
      </Panel>
    </section>}

    <section className="recon-layout">
      <Panel title="Position Reconciliation" right={<span className="muted">Tolerance: {data.tolerance}</span>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Target Qty</th><th>Effective Qty</th><th>Local Qty</th><th>Exchange Qty</th><th>Pending Qty</th><th>Exchange−Local</th><th>Status</th></tr></thead><tbody>
          {data.rows.length === 0 ? <tr><td colSpan={8} className="empty-cell">No positions or open-order quantities in the current durable state.</td></tr> : data.rows.map(row => <tr key={row.asset} title={row.explanation}><td><strong>{row.asset}</strong></td><td>{display(row, 'target')}</td><td>{display(row, 'effective')}</td><td>{display(row, 'local')}</td><td>{display(row, 'exchange')}</td><td>{display(row, 'pending')}</td><td className={row.deltaQty === 0 ? 'muted' : 'warning'}>{display(row, 'delta')}</td><td><StatePill state={row.status}/></td></tr>)}
        </tbody></table></div>
      </Panel>

      <Panel title="State Semantics">
        <div className="semantics-list">
          <div><StatePill state="ALIGNED"/><p>Current local and retained exchange evidence agree within the runtime tolerance.</p></div>
          <div><StatePill state="PENDING"/><p>Closed-loop proof is not current enough yet; stale/missing evidence is never promoted to CLEAN.</p></div>
          <div><StatePill state="DRIFT"/><p>Reserved for a non-blocking mismatch read model; the current C++ startup reconciler treats its issues as blocking.</p></div>
          <div><StatePill state="BLOCKED"/><p>Fresh evidence reproduces one or more C++ Reconciler mismatches.</p></div>
        </div>
      </Panel>
    </section>

    {real && <section className="recon-layout">
      <Panel title={`Reconciliation Issues (${issues.length})`} right={<span className="muted">Cash · positions · open orders</span>}>
        {issues.length === 0 ? <div className="empty-panel">No comparison issues in the retained evidence window.</div> : <div className="table-wrap"><table><thead><tr><th>Kind</th><th>Asset</th><th>Order</th><th>Local</th><th>Exchange</th><th>Message</th></tr></thead><tbody>{issues.map((issue, index) => <tr key={`${issue.kind}-${issue.orderId}-${issue.asset}-${index}`}><td><strong>{issue.kind}</strong></td><td>{issue.asset || '—'}</td><td className="mono">{issue.orderId || '—'}</td><td>{qty(issue.localValue)}</td><td>{qty(issue.exchangeValue)}</td><td>{issue.message}</td></tr>)}</tbody></table></div>}
      </Panel>
    </section>}

    <section className="recon-lower">
      <Panel title="Open Orders" right={<span className="muted">Durable local orders used by the reconciler</span>}>
        <div className="table-wrap"><table><thead><tr><th>Order ID</th><th>Asset</th><th>Side</th><th>Qty</th><th>Remaining</th><th>Last update</th><th>State</th><th>Exchange ID</th></tr></thead><tbody>
          {data.openOrders.length === 0 ? <tr><td colSpan={8} className="empty-cell">No locally open orders in the persisted runtime snapshot.</td></tr> : data.openOrders.map(order => <tr key={order.orderId}><td className="mono">{order.orderId}</td><td><strong>{order.asset}</strong></td><td className={order.side === 'BUY' ? 'positive' : 'negative'}>{order.side}</td><td>{qty(order.quantity)}</td><td>{qty(order.remaining)}</td><td>{order.age}</td><td>{order.state}</td><td className="mono muted">{order.exchangeId}</td></tr>)}
        </tbody></table></div>
      </Panel>
      <Panel title="Closed-loop Rule">
        <div className="callout callout--good"><strong>Dashboard observes; the trading pipeline remains authoritative.</strong><span>Step 19 uses a read-only JetStream direct-get of the latest retained exchange snapshot. It never publishes an exchange snapshot request and never writes to PostgreSQL, NATS trading subjects or the exchange.</span></div>
      </Panel>
    </section>
  </>
}
