import { Activity, CheckCircle2, Clock3, Database, SearchX, TriangleAlert } from 'lucide-react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { MarketDataData, MarketDataState } from '../types/dashboard'

function DataState({ state }: { state: MarketDataState }) {
  const tone = state === 'HEALTHY' || state === 'VALID' ? 'good' : state === 'WARN' ? 'warn' : 'bad'
  return <span className={`market-state market-state--${tone}`}>{state}</span>
}

function MarketView({ data }: { data: MarketDataData }) {
  const real = data.sourceMode === 'REAL'
  const integrityIssues = data.integrity.missingCandles + data.integrity.duplicateTimestamps + data.integrity.gaps + data.integrity.invalidRows
  const signalAligned = !real || data.signalCycleAligned === true

  return <>
    <div className="page-heading">
      <div><h1>Market Data</h1><p>Freshness, integrity, liquidity universe, indicators and candidate diagnostics.</p></div>
      <div className="cycle-label"><Clock3 size={14}/><span>Completed candle {data.latestCompletedCandle}</span></div>
    </div>

    {real && <div className="real-source-banner"><strong>REAL DATA</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Canonical frontier ${data.sourceUpdatedAt}.` : ''}</span></div>}

    <section className="market-summary">
      <div className="mini-metric"><span>Source</span><strong>{data.source}</strong><small>{real ? `Top-${data.canonicalTopN ?? '—'} canonical ranking` : 'Canonical market-data source'}</small></div>
      <div className="mini-metric"><span>Healthy assets</span><strong className={data.staleAssets === 0 ? 'positive' : 'warning'}>{data.healthyAssets}/{data.totalAssets}</strong><small>{data.staleAssets} stale</small></div>
      <div className="mini-metric"><span>Strategy universe</span><strong>{data.universeSize}</strong><small>{real ? `SMA Volume(25), ${data.historyDays ?? '—'}d bounded history` : 'Top liquidity candidates'}</small></div>
      <div className="mini-metric"><span>Active signals</span><strong>{signalAligned ? data.activeSignals : '—'}</strong><small>{signalAligned ? `${data.availableSlots} strategy slots free` : 'Strategy checkpoint not cycle-aligned'}</small></div>
      <div className="mini-metric"><span>Integrity</span><strong className={integrityIssues === 0 ? 'positive' : 'warning'}>{integrityIssues === 0 ? 'VALID' : 'WARN'}</strong><small>{data.integrity.missingCandles} missing · {data.integrity.gaps} gap runs</small></div>
    </section>

    <section className="market-grid">
      <Panel title="Freshness" right={<span className="muted">Last completed data by strategy-universe asset</span>}>
        <div className="table-wrap"><table><thead><tr><th>Asset</th><th>Last Candle</th><th>Age</th><th>Requirement</th><th>Source</th><th>Status</th></tr></thead><tbody>{data.freshness.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td>{row.lastCandle}</td><td className={row.state === 'STALE' ? 'negative' : ''}>{row.ageLabel}</td><td>{row.staleThresholdLabel}</td><td>{row.source}</td><td><DataState state={row.state}/></td></tr>)}</tbody></table></div>
      </Panel>

      <Panel title="Integrity" right={<span className={integrityIssues === 0 ? 'positive' : 'warning'}><CheckCircle2 size={12}/> canonical checks</span>}>
        <div className="integrity-grid"><div><Database size={16}/><span>Missing candles</span><strong>{data.integrity.missingCandles}</strong></div><div><Activity size={16}/><span>Duplicate timestamps</span><strong>{data.integrity.duplicateTimestamps}</strong></div><div><TriangleAlert size={16}/><span>Detected gap runs</span><strong>{data.integrity.gaps}</strong></div><div><SearchX size={16}/><span>Invalid price / volume</span><strong>{data.integrity.invalidRows}</strong></div></div>
        <div className="info-box">Data health and strategy choice are separate states.<small>{real ? 'Indicator diagnostics are recomputed from canonical SQLite; durable signals are joined only on an exact cycle match.' : 'A flat strategy state must not be confused with missing or stale market data.'}</small></div>
      </Panel>
    </section>

    <Panel title="Universe & Strategy Diagnostics" right={<span className="muted">SMA Volume 25 · RSI 7{real ? ' · canonical recompute' : ''}</span>}>
      <div className="table-wrap"><table className="wide-table market-table"><thead><tr><th>Rank</th><th>Asset</th><th>SMA Volume 25</th><th>RSI 7</th><th>Signal</th><th>Signal State</th><th>Slot</th><th>Diagnostic</th></tr></thead><tbody>{data.universe.map(row => <tr key={row.asset}><td>#{row.rank}</td><td><strong>{row.asset}</strong></td><td>{row.smaVolumeLabel}</td><td className={row.rsi >= 80 ? 'positive' : ''}>{row.rsi.toFixed(1)}</td><td><span className={`signal signal--${row.signal.toLowerCase()}`}>{row.signal}</span></td><td>{row.signalState}</td><td>{row.slotState}</td><td><span className={row.diagnosticTone === 'warn' ? 'warning' : row.diagnosticTone === 'bad' ? 'negative' : 'muted'}>{row.diagnostic}</span></td></tr>)}</tbody></table></div>
    </Panel>

    <section className="market-grid market-grid--bottom">
      <Panel title="Candidate Diagnostics" right={<span className="muted">Why a recomputed entry candidate is not active</span>}>
        <div className="candidate-list">
          {data.candidateRejections.length === 0 && <div className="empty-state">No candidate mismatch or signal-cap diagnostic for the current cycle.</div>}
          {data.candidateRejections.map(row => <div className="candidate-row" key={`${row.asset}-${row.reason}`}><strong>{row.asset}</strong><div><span>{row.reason}</span><small>{row.detail}</small></div><DataState state={row.severity}/></div>)}
        </div>
      </Panel>
      <Panel title="Strategy Snapshot" right={<span className="muted">verified PureRSI contract</span>}>
        <div className="snapshot-list"><div><span>Ranking method</span><strong>{data.strategySnapshot.ranking}</strong></div><div><span>Entry rule</span><strong>{data.strategySnapshot.entryRule}</strong></div><div><span>Exit rule</span><strong>{data.strategySnapshot.exitRule}</strong></div><div><span>Max positions</span><strong>{data.strategySnapshot.maxPositions}</strong></div><div><span>Semantics</span><strong>{data.strategySnapshot.semantics}</strong></div></div>
      </Panel>
    </section>
  </>
}

export function MarketDataPage() {
  const { data, error, retry } = useDashboardResource('getMarketData')

  if (error) return <DataLoadError title="Market data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading market data…</div>

  return <MarketView data={data}/>
}
