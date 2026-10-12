import { Link } from 'react-router-dom'
import { useMemo, useState } from 'react'
import { CloseLoopTimeline } from '../components/CloseLoopTimeline'
import { DataLoadError } from '../components/DataLoadError'
import { DAILY_PRESETS, DEFAULT_DAILY_RANGE, LineChart, chartDate, dailyWindow, displayAmount, usd } from '../components/LineChart'
import { Panel } from '../components/Panel'
import { StatePill } from '../components/StatePill'
import { TimeRangePicker } from '../components/TimeRangePicker'
import type { TimeRange } from '../components/TimeRangePicker'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { OperationalAlert, Position, Severity } from '../types/dashboard'

// Overview: account figures, balance/equity with the daily close loop, live equity against
// the fast backtest, open positions and the most important active alert. Units travel with
// the numbers; no explanatory captions.

const BALANCE = { key: 'balance', label: 'Balance', color: 'var(--series-1)' }   // blue
const EQUITY = { key: 'equity', label: 'Equity', color: 'var(--series-2)' }      // orange
const EXPECTED = { key: 'expected', label: 'Expected', color: 'var(--series-neutral)' }

const SEVERITY_RANK: Record<Severity, number> = { CRITICAL: 0, WARN: 1, INFO: 2 }

function signedUsd(value: number) {
  return `${value >= 0 ? '+' : '-'}${usd(Math.abs(value))}`
}

// PnL cards are green when >= 0 and red when negative. Leverage follows a fixed scale:
// green up to 1.5x, orange up to 2.5x, red above. Other cards stay neutral.
function statTone(label: string, value: string) {
  const amount = Number(value.replace(/[$,x\s]/g, ''))
  if (!Number.isFinite(amount) || !/\d/.test(value)) return ''
  if (/pnl/i.test(label)) return amount < 0 ? 'negative' : 'positive'
  if (/leverage/i.test(label)) return amount <= 1.5 ? 'positive' : amount <= 2.5 ? 'leverage--elevated' : 'negative'
  return ''
}

function notional(position: Position) {
  if (position.valuationAvailable === false || !Number.isFinite(position.currentPrice) || position.currentPrice <= 0) return '—'
  return usd(Math.abs(position.quantity * position.currentPrice))
}

const pnlKnown = (position: Position) => position.pnlAvailable !== false && Number.isFinite(position.pnlUsd)

// Most severe first, then most recent.
function orderAlerts(alerts: OperationalAlert[]) {
  return alerts
    .filter(alert => alert.status === 'ACTIVE')
    .sort((a, b) => SEVERITY_RANK[a.severity] - SEVERITY_RANK[b.severity] || b.timestamp.localeCompare(a.timestamp))
}

function AlertsPanel() {
  const { data } = useDashboardResource('getAlertsAudit')
  const alerts = orderAlerts(data?.alerts ?? [])
  const [top] = alerts
  const tone = top?.severity === 'CRITICAL' ? 'negative' : top?.severity === 'WARN' ? 'warning' : 'info'
  return <Panel title="Alerts" right={<Link className="text-link" to="/alerts-audit">{alerts.length > 1 ? `+${alerts.length - 1} more →` : 'Details →'}</Link>}>
    {top
      ? <div className="overview-alert">
          <strong className={tone}>{top.severity}</strong>
          <div><span>{top.title}</span><small>{top.service} · {top.timestamp}</small></div>
        </div>
      : <div className="empty-cell">No active alerts</div>}
  </Panel>
}

export function OverviewPage() {
  const { data, error, retry } = useDashboardResource('getOverview')
  // One time range for every chart on the page: presets, From/To or a drag-zoom on any chart.
  const [range, setRange] = useState<TimeRange>(DEFAULT_DAILY_RANGE)
  const days = useMemo(() => [...(data?.equityCurve ?? []), ...(data?.liveExpected ?? [])]
    .map(point => chartDate(point.label)).filter(Number.isFinite), [data])
  const closeLoop = useMemo(() => (data?.equityCurve ?? [])
    .map(point => ({ time: chartDate(point.label), clean: point.closeLoop ? point.closeLoop === 'CLEAN' : null }))
    .filter(row => Number.isFinite(row.time)).sort((a, b) => a.time - b.time), [data])
  if (error) return <DataLoadError title="Overview unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading overview…</div>

  const hasCloseLoop = closeLoop.some(row => row.clean !== null)
  const visible = days.length ? dailyWindow(range, Math.min(...days), Math.max(...days)) : { from: 0, to: 1 }
  const shared = { ...visible, onSelectRange: (from: number, to: number) => setRange({ kind: 'custom', from, to }),
    onReset: () => setRange(DEFAULT_DAILY_RANGE) }

  return <>
    <div className="page-heading">
      <h1>Overview</h1>
      <TimeRangePicker presets={DAILY_PRESETS} value={range} onChange={setRange} precision="date"/>
    </div>

    <section className="overview-stats">
      {data.stats.map(stat => <div className="overview-stat" key={stat.label}><span>{stat.label}</span><strong className={statTone(stat.label, stat.value)}>{displayAmount(stat.value)}</strong></div>)}
    </section>

    <section className="grid-2-1">
      <Panel title="Equity Curve">
        {data.equityCurve.length > 0
          ? <LineChart data={data.equityCurve} lines={[BALANCE, EQUITY]} shared={shared}/>
          : <div className="overview-unavailable"><strong>No history yet</strong></div>}
      </Panel>
      <Panel title="Live vs Expected" right={<Link className="text-link" to="/live-vs-expected">Details →</Link>}>
        {data.liveExpected.length > 0
          ? <LineChart data={data.liveExpected} lines={[EXPECTED, EQUITY]} compact shared={shared}/>
          : <div className="overview-unavailable overview-unavailable--compact"><strong>No comparison yet</strong></div>}
      </Panel>
    </section>

    {hasCloseLoop && <Panel title="Close loop" className="close-loop-panel">
      <CloseLoopTimeline times={closeLoop.map(row => row.time)} clean={closeLoop.map(row => row.clean)} {...shared}/>
    </Panel>}

    <section className="overview-bottom">
      <Panel title="Active Positions" right={<Link className="text-link" to="/positions">View all →</Link>}>
        <div className="table-wrap"><table><thead><tr><th>Asset</th><th>Side</th><th>Notional</th><th>PnL</th><th>Status</th></tr></thead><tbody>
          {data.positions.length === 0
            ? <tr><td colSpan={5} className="empty-cell">No open positions</td></tr>
            : data.positions.map(position => <tr key={position.asset}>
                <td><strong>{position.asset}</strong></td>
                <td className={position.side === 'Short' ? 'negative' : 'positive'}>{position.side}</td>
                <td>{notional(position)}</td>
                <td className={pnlKnown(position) ? (position.pnlUsd < 0 ? 'negative' : 'positive') : ''}>{pnlKnown(position) ? signedUsd(position.pnlUsd) : '—'}</td>
                <td><StatePill state={position.status}/></td>
              </tr>)}
        </tbody></table></div>
      </Panel>
      <AlertsPanel/>
    </section>
  </>
}
