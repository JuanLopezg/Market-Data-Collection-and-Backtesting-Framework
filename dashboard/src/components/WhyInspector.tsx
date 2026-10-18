import { ArrowDownRight, Database, Fingerprint, Gauge, GitBranch, Scale, ShoppingCart, X } from 'lucide-react'
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

// The full daily cycle of one asset, step by step: market close, indicator and signal,
// sizing (equal weight or volatility target, whichever the strategy uses), risk caps, the
// rebalance band, the order plan, execution and reconciliation with the exchange.
export function WhyInspector({ trace, onClose }: WhyInspectorProps) {
  const { strategy, portfolio } = trace
  return <aside className="why-inspector">
    <div className="why-inspector__header">
      <div>
        <span className="eyebrow">Trace · full trading cycle</span>
        <h2>{trace.asset} · {trace.cycleId}</h2>
      </div>
      <button className="icon-button" onClick={onClose} aria-label="Close trace"><X size={18}/></button>
    </div>

    <div className="why-inspector__meta">
      <div><span>Decision</span><strong>{trace.audit.decisionTimestamp}</strong></div>
      <div><span>Execution</span><strong>{trace.audit.executionTimestamp}</strong></div>
      <div><span>Mode</span><strong>{trace.audit.mode}</strong></div>
      <div><span>Exchange</span><strong>{trace.audit.exchange}</strong></div>
    </div>

    <div className="why-inspector__scroll">
      <Block title="1 · Market" icon={<Database size={15}/>}>
        <Pair label="Close" value={trace.market.price}/><Pair label="Universe" value={trace.market.universe}/><Pair label="Liquidity rank" value={trace.market.liquidityRank}/><Pair label="Freshness" value={trace.market.freshness}/>
      </Block>
      <Block title="2 · Strategy" icon={<GitBranch size={15}/>}>
        {strategy.rule && <Pair label="Rule" value={strategy.rule}/>}
        <Pair label="Indicator" value={strategy.rsi}/><Pair label="Signal" value={strategy.signal}/><Pair label="Transition" value={strategy.transition}/><Pair label="Intent" value={strategy.intent}/>
      </Block>
      <Block title="3 · Sizing and risk" icon={<Scale size={15}/>}>
        {portfolio.sizer && <Pair label="Sizer" value={portfolio.sizer}/>}
        <Pair label="Sized target" value={portfolio.rawTarget}/>
        {portfolio.volTarget && <Pair label="Vol targeting" value={portfolio.volTarget}/>}
        <div className="why-list"><span>Risk caps</span>{portfolio.riskChanges.map(item => <strong key={item}>{item}</strong>)}</div>
        {portfolio.rebalance && <Pair label="Rebalance" value={portfolio.rebalance}/>}
        <Pair label="Decision" value={portfolio.approvedTarget}/>
      </Block>
      <Block title="4 · Planning" icon={<Gauge size={15}/>}>
        <Pair label="Effective qty" value={trace.planning.effectiveQty}/><Pair label="Pending qty" value={trace.planning.pendingQty}/><Pair label="Required delta" value={trace.planning.delta}/><Pair label="State revision" value={<code>{trace.planning.stateRevision}</code>}/>
      </Block>
      <Block title="5 · Execution" icon={<ShoppingCart size={15}/>}>
        <Pair label="Order IDs" value={trace.execution.orderIds.length ? trace.execution.orderIds.join(', ') : '—'}/><Pair label="Side" value={trace.execution.side}/><Pair label="Quantity" value={trace.execution.quantity}/><Pair label="Fill state" value={trace.execution.fillState}/><Pair label="Fees" value={trace.execution.fees}/><Pair label="Slippage" value={trace.execution.slippage}/>
      </Block>
      <Block title="6 · Reconciliation" icon={<ArrowDownRight size={15}/>}>
        <Pair label="Local qty" value={trace.reconciliation.localQty}/><Pair label="Exchange qty" value={trace.reconciliation.exchangeQty}/><Pair label="Status" value={<StatePill state={trace.reconciliation.status}/>}/>
      </Block>
      <Block title="Audit" icon={<Fingerprint size={15}/>}>
        <Pair label="Correlation ID" value={<code>{trace.audit.correlationId}</code>}/><Pair label="Config version" value={<code>{trace.audit.configVersion}</code>}/>
      </Block>
    </div>
  </aside>
}
