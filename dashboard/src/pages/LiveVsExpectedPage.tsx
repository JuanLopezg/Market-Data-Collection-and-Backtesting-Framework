import { Activity, BarChart3, Gauge, Info, Search, Sigma, TriangleAlert } from 'lucide-react'
import { useMemo, useState } from 'react'
import { BaselineTrendChart, DistributionBars } from '../components/BaselineTrendChart'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { BehaviourClassification, BehaviourMetric, LiveVsExpectedData } from '../types/dashboard'

function classificationTone(value: BehaviourClassification) {
  if (value === 'NORMAL') return 'good'
  if (value === 'ELEVATED') return 'warn'
  if (value === 'ABNORMAL') return 'bad'
  return 'critical'
}

function coverageTone(value: string) {
  if (value === 'VALIDATED') return 'good'
  if (value === 'DEFERRED' || value === 'INSUFFICIENT_DATA') return 'warn'
  return 'critical'
}

function ZScore({ value }: { value: number }) {
  const abs = Math.abs(value)
  const tone = abs >= 3 ? 'bad' : abs >= 2 ? 'warn' : abs >= 1 ? 'info' : 'good'
  return <span className={`zscore zscore--${tone}`}>{value >= 0 ? '+' : ''}{value.toFixed(1)}σ</span>
}

function MetricCard({ metric, selected, onSelect }: { metric: BehaviourMetric; selected: boolean; onSelect: () => void }) {
  const tone = classificationTone(metric.classification)
  return <button type="button" className={`behaviour-card ${selected ? 'behaviour-card--selected' : ''}`} onClick={onSelect}>
    <div className="behaviour-card__header"><span>{metric.family}</span><span className={`behaviour-class behaviour-class--${tone}`}>{metric.classification}</span></div>
    <div className="behaviour-card__metric"><div><strong>{metric.currentLabel}</strong><span>{metric.label}</span></div><ZScore value={metric.zScore}/></div>
    <BaselineTrendChart data={metric.trend} compact />
    <div className="behaviour-card__baseline"><span>Historical mean <strong>{metric.historicalMeanLabel}</strong></span><span>Range <strong>{metric.historicalRangeLabel}</strong></span></div>
  </button>
}

export function LiveVsExpectedPage() {
  const { data, error, retry } = useDashboardResource('getLiveVsExpected')
  const [selectedId, setSelectedId] = useState('entry-qualified')
  const [family, setFamily] = useState('ALL')
  const [query, setQuery] = useState('')


  const filtered = useMemo(() => {
    if (!data) return []
    return data.metrics.filter(metric => (family === 'ALL' || metric.family === family) && `${metric.label} ${metric.family}`.toLowerCase().includes(query.toLowerCase()))
  }, [data, family, query])

  const selected = data?.metrics.find(metric => metric.id === selectedId) ?? filtered[0] ?? null

  if (error) return <DataLoadError title="Live behaviour data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading live behaviour baseline…</div>

  return <>
    <div className="page-heading">
      <div><h1>Live vs Expected</h1><p>Behaviour monitoring against historical distributions — anomaly detection, not an expected-PnL promise.</p></div>
      <div className="baseline-meta"><span>Baseline</span><strong>{data.baselineLabel}</strong><small>{data.baselineWindow}</small></div>
    </div>

    <div className="behaviour-note"><Info size={15}/><div><strong>{data.sourceMode === 'REAL' ? 'Real baseline interpretation' : 'Interpretation'}</strong><span>{data.sourceNote ?? 'A deviation means live operating behaviour differs from the historical baseline. It does not by itself mean the strategy is profitable, unprofitable, broken or unsafe.'}</span></div></div>

    <Panel title="Step 45 · Real Projection Contract" right={<span className={`behaviour-class behaviour-class--${data.validated ? 'good' : 'critical'}`}>{data.status}</span>}>
      <div className="readiness-grid">
        <div><span>Contract</span><strong>{data.contractVersion}</strong></div>
        <div><span>Observation</span><strong>{data.observationMode.split('_').join(' ')}</strong></div>
        <div><span>Baseline observations</span><strong>{data.baselineObservationCount}</strong></div>
        <div><span>Overall classification</span><strong>{data.overallClassification}</strong></div>
        <div><span>Anomalies</span><strong>{data.anomalyCount}</strong></div>
        <div><span>Safety</span><strong>{data.readOnly ? 'READ-ONLY' : 'INVALID'} · routing {data.orderRouting}</strong></div>
      </div>
      <div className="muted" style={{marginTop: 10}}>Baseline fingerprint: <code>{data.baselineFingerprint}</code></div>
      <div className="dependency-list" style={{marginTop: 12}}>{data.coverage.map(row => <div className="dependency-row" key={row.id}><div><strong>{row.label}</strong><small>{row.detail}</small></div><span>{row.source}</span><b className={`behaviour-class behaviour-class--${coverageTone(row.state)}`}>{row.state}</b></div>)}</div>
    </Panel>

    {data.anomalies.length > 0 && <Panel title="Current Anomaly Projection" right={<span className="muted">Observation only · no routing action</span>}>
      <div className="dependency-list">{data.anomalies.map(anomaly => <div className="dependency-row" key={anomaly.metricId}><div><strong>{anomaly.label}</strong><small>{anomaly.detail}</small></div><span>{anomaly.currentLabel} · {anomaly.zScore >= 0 ? '+' : ''}{anomaly.zScore.toFixed(2)}σ</span><b className={`behaviour-class behaviour-class--${classificationTone(anomaly.classification)}`}>{anomaly.classification}</b></div>)}</div>
    </Panel>}

    <section className="behaviour-summary">
      <div className="mini-metric"><span>Metrics</span><strong>{data.metricCount}</strong><small>Latest {data.latestObservation}</small></div>
      <div className="mini-metric"><span>Normal</span><strong className="positive">{data.normalCount}</strong><small>Inside expected range</small></div>
      <div className="mini-metric"><span>Elevated</span><strong className="warning">{data.elevatedCount}</strong><small>Worth observing</small></div>
      <div className="mini-metric"><span>Abnormal</span><strong className="negative">{data.abnormalCount}</strong><small>Investigate context</small></div>
      <div className="mini-metric"><span>Critical</span><strong className={data.criticalCount ? 'negative' : 'positive'}>{data.criticalCount}</strong><small>Hard deviation</small></div>
    </section>

    <Panel title="Behaviour Metrics" right={<span className="muted">Select a card for drill-down</span>}>
      <div className="behaviour-toolbar">
        <div className="search-box"><Search size={13}/><input value={query} onChange={event => setQuery(event.target.value)} placeholder="Search metric…" /></div>
        <label>Family<select value={family} onChange={event => setFamily(event.target.value)}><option>ALL</option><option>ACTIVITY</option><option>HOLDING</option><option>RISK</option><option>PERFORMANCE</option><option>EXECUTION</option><option>STRATEGY INPUTS</option></select></label>
      </div>
      <div className="behaviour-card-grid">
        {filtered.map(metric => <MetricCard key={metric.id} metric={metric} selected={metric.id === selected?.id} onSelect={() => setSelectedId(metric.id)} />)}
      </div>
    </Panel>

    {selected && <section className="behaviour-drilldown">
      <Panel title={`${selected.label} — Live history vs baseline`} right={<div className="behaviour-detail-status"><ZScore value={selected.zScore}/><span className={`behaviour-class behaviour-class--${classificationTone(selected.classification)}`}>{selected.classification}</span></div>}>
        <div className="behaviour-detail-heading"><div><span className="eyebrow">{selected.family}</span><strong>{selected.currentLabel}</strong><p>{selected.description}</p></div><div className="behaviour-detail-kpis"><span>Historical mean<strong>{selected.historicalMeanLabel}</strong></span><span>Historical range<strong>{selected.historicalRangeLabel}</strong></span></div></div>
        <BaselineTrendChart data={selected.trend}/>
        <div className="behaviour-chart-legend"><span><i className="legend-live"/>Live</span><span><i className="legend-mean"/>Historical mean</span><span><i className="legend-band"/>Baseline envelope</span></div>
      </Panel>

      <div className="behaviour-side-stack">
        <Panel title="Historical Distribution" right={<BarChart3 size={14}/>}>
          <div className="distribution-wrap"><DistributionBars values={selected.distribution}/><div className="distribution-axis"><span>Low</span><span>Mean</span><span>High</span></div><div className="distribution-current"><Sigma size={13}/><span>Current observation</span><strong><ZScore value={selected.zScore}/></strong></div></div>
        </Panel>
        <Panel title="Recent Contributing Cycles" right={<Activity size={14}/>}>
          <div className="contributor-list">{selected.contributingCycles.map(cycle => <div className="contributor-row" key={cycle.cycleId}><div><strong>{cycle.asset}</strong><small>{cycle.timestamp}</small></div><code>{cycle.cycleId}</code><span>{cycle.valueLabel}</span><b>{cycle.deviationLabel}</b></div>)}</div>
        </Panel>
      </div>
    </section>}

    <div className="behaviour-footer-hint"><TriangleAlert size={14}/><span>Step 45 is intentionally LIMITED to canonical market/strategy-input distributions. Execution, accounting/PnL and accepted-replay baselines remain explicit DEFERRED coverage until canonical versioned artifacts exist.</span><Gauge size={14}/></div>
  </>
}
