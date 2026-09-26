import { ArrowDownRight, Database, Fingerprint, Gauge, GitBranch, ReceiptText, Scale, ShoppingCart, X } from 'lucide-react'
import type { WhyTrace } from '../types/dashboard'
import { StatePill } from './StatePill'

interface WhyInspectorProps {
  trace: WhyTrace
  onClose: () => void
}

function Block({ title, icon, children }: { title: string; icon: React.ReactNode; children: React.ReactNode }) {
  return <section className="why-block">
    <div className="why-block__title">{icon}<strong>{title}</strong></div>
    {children}
  </section>
}

function Pair({ label, value }: { label: string; value: React.ReactNode }) {
  return <div className="why-pair"><span>{label}</span><strong>{value}</strong></div>
}

export function WhyInspector({ trace, onClose }: WhyInspectorProps) {
  return <aside className="why-inspector">
    <div className="why-inspector__header">
      <div>
        <span className="eyebrow">Why? · Trading Cycle Inspector</span>
        <h2>{trace.asset} · {trace.cycleId}</h2>
      </div>
      <button className="icon-button" onClick={onClose} aria-label="Close inspector"><X size={18}/></button>
    </div>

    <div className="why-inspector__meta">
      <div><span>Decision</span><strong>{trace.audit.decisionTimestamp}</strong></div>
      <div><span>Execution</span><strong>{trace.audit.executionTimestamp}</strong></div>
      <div><span>Mode</span><strong>{trace.audit.mode}</strong></div>
      <div><span>Exchange</span><strong>{trace.audit.exchange}</strong></div>
    </div>

    <div className="why-inspector__scroll">
      <Block title="Market" icon={<Database size={15}/>}>
        <Pair label="Price" value={trace.market.price}/><Pair label="Universe" value={trace.market.universe}/><Pair label="Liquidity rank" value={trace.market.liquidityRank}/><Pair label="Freshness" value={trace.market.freshness}/>
      </Block>
      <Block title="Strategy" icon={<GitBranch size={15}/>}>
        <Pair label="RSI(7)" value={trace.strategy.rsi}/><Pair label="Signal" value={trace.strategy.signal}/><Pair label="Transition" value={trace.strategy.transition}/><Pair label="Intent" value={trace.strategy.intent}/>
      </Block>
      <Block title="Portfolio / Risk" icon={<Scale size={15}/>}>
        <Pair label="Raw target" value={trace.portfolio.rawTarget}/><Pair label="Vol target" value={trace.portfolio.volTarget}/><Pair label="Approved" value={trace.portfolio.approvedTarget}/>
        <div className="why-list"><span>Transformations</span>{trace.portfolio.riskChanges.map(item => <strong key={item}>{item}</strong>)}</div>
      </Block>
      <Block title="Planning" icon={<Gauge size={15}/>}>
        <Pair label="Effective qty" value={trace.planning.effectiveQty}/><Pair label="Pending qty" value={trace.planning.pendingQty}/><Pair label="Required delta" value={trace.planning.delta}/><Pair label="State revision" value={<code>{trace.planning.stateRevision}</code>}/>
      </Block>
      <Block title="Execution" icon={<ShoppingCart size={15}/>}>
        <Pair label="Order IDs" value={trace.execution.orderIds.length ? trace.execution.orderIds.join(', ') : '—'}/><Pair label="Side" value={trace.execution.side}/><Pair label="Quantity" value={trace.execution.quantity}/><Pair label="Fill state" value={trace.execution.fillState}/><Pair label="Fees" value={trace.execution.fees}/><Pair label="Slippage" value={trace.execution.slippage}/>
      </Block>
      <Block title="Reconciliation" icon={<ArrowDownRight size={15}/>}>
        <Pair label="Local qty" value={trace.reconciliation.localQty}/><Pair label="Exchange qty" value={trace.reconciliation.exchangeQty}/><Pair label="Status" value={<StatePill state={trace.reconciliation.status}/>}/>
      </Block>
      <Block title="Audit" icon={<Fingerprint size={15}/>}>
        <Pair label="Correlation ID" value={<code>{trace.audit.correlationId}</code>}/><Pair label="Config version" value={<code>{trace.audit.configVersion}</code>}/>
        <div className="raw-event-hint"><ReceiptText size={14}/> Raw event/log links will be connected when the real observability backend exists.</div>
      </Block>
    </div>
  </aside>
}
