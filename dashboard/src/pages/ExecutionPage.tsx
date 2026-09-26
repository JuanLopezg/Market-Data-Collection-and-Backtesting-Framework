import { AlertTriangle, ArrowRightLeft, Clock3, Filter, Search, X } from 'lucide-react'
import { useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { ExecutionData, ExecutionOrder, ExecutionOrderState } from '../types/dashboard'

function OrderState({ state }: { state: ExecutionOrderState }) {
  const tone = state === 'FILLED' ? 'good' : state === 'REJECTED' ? 'bad' : state === 'NEW' || state === 'PARTIAL' || state === 'PENDING_CANCEL' ? 'warn' : 'muted'
  return <span className={`order-state order-state--${tone}`}>{state}</span>
}

function Side({ side }: { side: 'BUY' | 'SELL' }) {
  return <span className={side === 'BUY' ? 'positive' : 'negative'}>{side}</span>
}

function fmtQty(value: number) {
  return value.toLocaleString(undefined, { maximumFractionDigits: 6 })
}

function fmtMoney(value: number) {
  const sign = value > 0 ? '+' : value < 0 ? '-' : ''
  return `${sign}$${Math.abs(value).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`
}

function OrderInspector({ order, onClose, isReal }: { order: ExecutionOrder; onClose: () => void; isReal: boolean }) {
  return <>
    <button className="inspector-backdrop inspector-backdrop--button" aria-label="Close order inspector" onClick={onClose}/>
    <aside className="order-inspector">
      <div className="order-inspector__header">
        <div><span className="eyebrow">Execution order</span><h2>{order.asset} · {order.orderId}</h2></div>
        <button className="icon-button" onClick={onClose} aria-label="Close"><X size={16}/></button>
      </div>
      <div className="order-inspector__summary">
        <div><span>State</span><OrderState state={order.state}/></div>
        <div><span>Side</span><strong><Side side={order.side}/></strong></div>
        <div><span>Filled</span><strong>{fmtQty(order.filledQty)} / {fmtQty(order.quantity)}</strong></div>
        <div><span>Exchange ID</span><code>{order.exchangeOrderId || '—'}</code></div>
      </div>
      <div className="order-inspector__scroll">
        <section className="why-block">
          <div className="why-block__title"><ArrowRightLeft size={14}/><strong>Order economics</strong></div>
          <div className="why-pair"><span>Expected price</span><strong>{order.expectedPriceLabel}</strong></div>
          <div className="why-pair"><span>Average fill</span><strong>{order.avgFillPriceLabel}</strong></div>
          <div className="why-pair"><span>Fees</span><strong>{order.feesLabel}</strong></div>
          <div className="why-pair"><span>Slippage</span><strong className={order.slippageBps > 2 ? 'warning' : ''}>{order.slippageBpsLabel}</strong></div>
        </section>
        <section className="why-block">
          <div className="why-block__title"><Clock3 size={14}/><strong>Latency</strong></div>
          <div className="why-pair"><span>Submit latency</span><strong>{isReal ? 'Not persisted' : `${order.submitLatencyMs} ms`}</strong></div>
          <div className="why-pair"><span>Fill latency</span><strong>{isReal ? 'Not persisted' : order.fillLatencyMs === null ? 'Pending' : `${order.fillLatencyMs} ms`}</strong></div>
          <div className="why-pair"><span>{isReal ? 'Created' : 'Submitted'}</span><strong>{order.submittedAt}</strong></div>
          <div className="why-pair"><span>Last update</span><strong>{order.lastUpdateAt}</strong></div>
        </section>
        <section className="why-block">
          <div className="why-block__title"><Filter size={14}/><strong>Traceability</strong></div>
          <div className="why-pair"><span>Strategy</span><code>{order.strategyId ?? '—'}</code></div>
          <div className="why-pair"><span>Cycle</span><code>{order.cycleId}</code></div>
          <div className="why-pair"><span>Correlation ID</span><code>{order.correlationId}</code></div>
          <div className="why-pair"><span>Client order ID</span><code>{order.orderId}</code></div>
        </section>
        <section className="why-block">
          <div className="why-block__title"><Clock3 size={14}/><strong>Lifecycle</strong></div>
          <div className="execution-timeline">{order.lifecycle.map((event, index) => <div className="execution-timeline__event" key={`${event.time}-${event.state}-${index}`}>
            <span className={`timeline-dot timeline-dot--${event.tone}`}/><time>{event.time}</time><div><strong>{event.state}</strong><small>{event.detail}</small></div>
          </div>)}</div>
        </section>
      </div>
    </aside>
  </>
}

export function ExecutionPage() {
  const { data, error, retry } = useDashboardResource('getExecution')
  const { data: ledger, error: ledgerError, retry: retryLedger } = useDashboardResource('getLedger')
  const [query, setQuery] = useState('')
  const [selected, setSelected] = useState<ExecutionOrder | null>(null)
  const [stateFilter, setStateFilter] = useState<'ALL' | ExecutionOrderState>('ALL')


  const orders = useMemo(() => {
    if (!data) return []
    const q = query.trim().toLowerCase()
    return data.orders.filter(order => {
      const matchesQuery = !q || `${order.orderId} ${order.exchangeOrderId} ${order.asset} ${order.correlationId}`.toLowerCase().includes(q)
      const matchesState = stateFilter === 'ALL' || order.state === stateFilter
      return matchesQuery && matchesState
    })
  }, [data, query, stateFilter])

  if (error) return <DataLoadError title="Execution unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading execution…</div>

  const isReal = data.sourceMode === 'REAL'

  return <>
    <div className="page-heading">
      <div><h1>Execution</h1><p>Orders, fills, rejects, replacements, latency and realized execution quality.</p></div>
      <div className="cycle-label"><Clock3 size={14}/><span>Updated {data.lastUpdated}</span></div>
    </div>

    {isReal && <div className="real-source-banner"><strong>REAL DATA</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Persisted ${data.sourceUpdatedAt}.` : ''}</span></div>}

    <section className="execution-summary">
      <div className="mini-metric"><span>Open orders</span><strong>{data.openOrders}</strong><small>{data.partialOrders} partial · {data.pendingCancels} pending cancel</small></div>
      <div className="mini-metric"><span>{isReal ? 'Filled orders' : 'Filled this cycle'}</span><strong>{data.filledOrders}</strong><small>{data.fillCount} {isReal ? 'persisted fill rows' : 'fills received'}</small></div>
      <div className="mini-metric"><span>Rejected</span><strong className={data.rejectCount ? 'warning' : ''}>{data.rejectCount}</strong><small>{isReal ? 'Tracked-order window' : 'Current execution window'}</small></div>
      <div className="mini-metric"><span>Avg submit latency</span><strong>{isReal && !data.latencyAvailable ? '—' : `${data.avgSubmitLatencyMs} ms`}</strong><small>{isReal ? 'Not persisted in runtime snapshot' : 'P50 service → venue'}</small></div>
      <div className="mini-metric"><span>Avg fill latency</span><strong>{isReal && !data.latencyAvailable ? '—' : `${data.avgFillLatencyMs} ms`}</strong><small>{isReal ? 'No wall-clock execution trace' : 'Submitted → first fill'}</small></div>
      <div className="mini-metric"><span>Avg slippage</span><strong className={isReal ? 'muted' : 'positive'}>{data.avgSlippageBpsLabel}</strong><small>{isReal ? 'Expected price not persisted' : 'Expected vs realized'}</small></div>
    </section>

    <Panel title="Step 42 · Append-Only Ledger Foundation" right={<span className={ledger?.validated ? 'positive' : ledgerError || ledger?.error ? 'negative' : 'muted'}>{ledger?.validated ? 'VALIDATED' : ledger?.status ?? 'LOADING'}</span>}>
      {ledger ? <>
        <div className="execution-summary">
          <div className="mini-metric"><span>Durable fill events</span><strong>{ledger.totalFillRows}</strong><small>{ledger.distinctFillIds} distinct fill IDs</small></div>
          <div className="mini-metric"><span>Total fees</span><strong>{ledger.totalFeesLabel}</strong><small>Persisted commission</small></div>
          <div className="mini-metric"><span>Gross buys</span><strong>{fmtMoney(-ledger.grossBuyNotional)}</strong><small>Economic cash outflow before fees</small></div>
          <div className="mini-metric"><span>Gross sells</span><strong>{fmtMoney(ledger.grossSellNotional)}</strong><small>Economic cash inflow before fees</small></div>
          <div className="mini-metric"><span>Net cash delta</span><strong className={ledger.netCashDeltaFromFills >= 0 ? 'positive' : 'negative'}>{fmtMoney(ledger.netCashDeltaFromFills)}</strong><small>Fills only · includes commission</small></div>
          <div className="mini-metric"><span>Accounting scope</span><strong className="warning">FOUNDATION</strong><small>Realized PnL / cost basis not yet durable</small></div>
        </div>
        {ledger.error && <div className="negative" style={{marginBottom: 10}}><strong>Ledger blocker:</strong> {ledger.error}</div>}
        <div className="muted" style={{marginBottom: 10}}>{ledger.note}</div>
        <div className="table-wrap"><table className="wide-table"><thead><tr><th>Entry</th><th>Fill</th><th>Order</th><th>Asset</th><th>Side</th><th>Qty Δ</th><th>Cash Δ</th><th>Fee</th><th>Time</th><th>Hash</th></tr></thead><tbody>{ledger.entries.map(entry => <tr key={entry.entryId}><td className="mono">{entry.entryId}</td><td className="mono">{entry.fillId}</td><td className="mono">{entry.orderId}</td><td><strong>{entry.asset}</strong></td><td><Side side={entry.side}/></td><td className={entry.positionDelta >= 0 ? 'positive' : 'negative'}>{entry.positionDelta >= 0 ? '+' : ''}{fmtQty(entry.positionDelta)}</td><td className={entry.cashDelta >= 0 ? 'positive' : 'negative'}>{fmtMoney(entry.cashDelta)}</td><td>{fmtMoney(-entry.commission)}</td><td>{entry.timestamp}</td><td className="mono">{entry.entryHash.slice(0, 12)}…</td></tr>)}</tbody></table>{ledger.entries.length === 0 && <div className="empty-state">No persisted fills yet. An empty append-only ledger is valid at this stage.</div>}</div>
        <div className="muted" style={{marginTop: 8}}>Recent fingerprint {ledger.recentWindowFingerprint.slice(0, 16)}… · window {ledger.recentWindowCount}{ledger.recentWindowTruncated ? ` of ${ledger.totalFillRows}` : ''} · {ledger.sourceContract}</div>
      </> : <div className="muted">{ledgerError ? <><span>Ledger foundation unavailable. </span><button className="link-button" onClick={retryLedger}>Retry</button></> : 'Loading append-only fill ledger foundation…'}</div>}
    </Panel>

    <Panel title="Orders" right={<div className="table-actions">
      <div className="search-box"><Search size={13}/><input value={query} onChange={e => setQuery(e.target.value)} placeholder="Order, asset, correlation…"/></div>
      <select className="compact-select" value={stateFilter} onChange={e => setStateFilter(e.target.value as 'ALL' | ExecutionOrderState)}><option>ALL</option><option>NEW</option><option>PARTIAL</option><option>FILLED</option><option>PENDING_CANCEL</option><option>CANCELED</option><option>REJECTED</option></select>
    </div>}>
      <div className="table-wrap"><table className="wide-table execution-table"><thead><tr><th>Order</th><th>Asset</th><th>Side</th><th>Qty</th><th>Filled</th><th>Remaining</th><th>State</th><th>Age</th><th>Avg Fill</th><th>Fees</th><th>Slippage</th><th>Latency</th></tr></thead><tbody>{orders.map(order => <tr className="clickable-row" key={order.orderId} onClick={() => setSelected(order)}>
        <td><strong className="mono">{order.orderId}</strong><span className="cell-sub mono">{order.exchangeOrderId || 'No exchange id'}</span></td><td><strong>{order.asset}</strong></td><td><Side side={order.side}/></td><td>{fmtQty(order.quantity)}</td><td>{fmtQty(order.filledQty)}</td><td>{fmtQty(order.remainingQty)}</td><td><OrderState state={order.state}/></td><td>{order.age}</td><td>{order.avgFillPriceLabel}</td><td>{order.feesLabel}</td><td className={order.slippageBps > 2 ? 'warning' : order.slippageBps < 0 ? 'positive' : ''}>{order.slippageBpsLabel}</td><td>{isReal && !data.latencyAvailable ? '—' : `${order.submitLatencyMs} ms`}<span className="cell-sub">{isReal ? 'not persisted' : 'submit'}</span></td>
      </tr>)}</tbody></table>{orders.length === 0 && <div className="empty-state">No orders match the current filters.</div>}</div>
    </Panel>

    <section className="execution-grid">
      <Panel title={isReal ? "Persisted Fills" : "Partial Fills"} right={<span className="muted">{isReal ? "trading_fills · bounded window" : "Cumulative fill state"}</span>}>
        <div className="table-wrap"><table><thead><tr><th>Fill ID</th><th>Order</th><th>Asset</th><th>Qty</th><th>Price</th><th>Cumulative</th><th>Fees</th><th>Time</th></tr></thead><tbody>{data.partialFills.map(fill => <tr key={fill.fillId}><td className="mono">{fill.fillId}</td><td className="mono">{fill.orderId}</td><td><strong>{fill.asset}</strong></td><td>{fmtQty(fill.quantity)}</td><td>{fill.priceLabel}</td><td>{fill.cumulativeLabel}</td><td>{fill.feesLabel}</td><td>{fill.timestamp}</td></tr>)}</tbody></table>{data.partialFills.length === 0 && <div className="empty-state">{isReal ? 'No persisted fills for the displayed tracked orders.' : 'No fills in this window.'}</div>}</div>
      </Panel>
      <Panel title="Rejects" right={data.rejects.length ? <span className="warning"><AlertTriangle size={12}/> attention</span> : <span className="positive">None</span>}>
        {data.rejects.length ? <div className="reject-list">{data.rejects.map(reject => <div className="reject-card" key={reject.orderId}><div><strong>{reject.asset} · {reject.orderId}</strong><span>{reject.code}</span></div><p>{reject.reason}</p><small>{reject.remediation}</small></div>)}</div> : <div className="empty-state">No rejected orders in this window.</div>}
      </Panel>
    </section>

    <section className="execution-grid execution-grid--bottom">
      <Panel title="Cancels / Replacements" right={<span className="muted">{isReal ? "replacement lineage not persisted" : "Order lifecycle continuity"}</span>}>
        <div className="table-wrap"><table><thead><tr><th>Asset</th><th>Original</th><th>Cancel requested</th><th>Canceled</th><th>Replacement</th><th>State</th></tr></thead><tbody>{data.replacements.map(row => <tr key={row.originalOrderId}><td><strong>{row.asset}</strong></td><td className="mono">{row.originalOrderId}</td><td>{row.cancelRequestedAt}</td><td>{row.canceledAt}</td><td className="mono">{row.replacementOrderId}</td><td><OrderState state={row.replacementState}/></td></tr>)}</tbody></table>{data.replacements.length === 0 && <div className="empty-state">{isReal ? 'No canonical replacement linkage is persisted in the current runtime contract.' : 'No cancels or replacements in this window.'}</div>}</div>
      </Panel>
      <Panel title="Execution Quality" right={<span className="muted">Current cycle</span>}>
        <div className="quality-grid"><div><span>Submit latency P95</span><strong>{isReal && !data.latencyAvailable ? '—' : `${data.submitLatencyP95Ms} ms`}</strong></div><div><span>Fill latency P95</span><strong>{isReal && !data.latencyAvailable ? '—' : `${data.fillLatencyP95Ms} ms`}</strong></div><div><span>Best slippage</span><strong className={isReal ? 'muted' : 'positive'}>{data.bestSlippageBpsLabel}</strong></div><div><span>Worst slippage</span><strong className={isReal ? 'muted' : 'warning'}>{data.worstSlippageBpsLabel}</strong></div><div><span>{isReal ? 'Persisted fees' : 'Total fees'}</span><strong>{data.totalFeesLabel}</strong></div><div><span>Reject rate</span><strong>{data.rejectRateLabel}</strong></div></div>
      </Panel>
    </section>

    {selected && <OrderInspector order={selected} isReal={isReal} onClose={() => setSelected(null)}/>} 
  </>
}
