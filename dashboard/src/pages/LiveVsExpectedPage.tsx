import { useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { DAILY_PRESETS, DEFAULT_DAILY_RANGE, LineChart, chartDate, dailyWindow, usd } from '../components/LineChart'
import { TimeRangePicker } from '../components/TimeRangePicker'
import type { TimeRange } from '../components/TimeRangePicker'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { LiveVsExpectedRow } from '../types/dashboard'

// Live vs Expected: does the running system earn what the fast backtest says it should?
// Equity against the expected equity for the same days, the gap over time, and (PAPER)
// the positions against the expected positions.

const EQUITY = { key: 'equity', label: 'Equity', color: 'var(--series-2)' }
const EXPECTED = { key: 'expected', label: 'Expected', color: 'var(--series-neutral)' }
const GAP = { key: 'gap', label: 'Difference', color: 'var(--series-1)' }
const PAGE = 20
// A gap inside ±0.5% is treated as matching; beyond ±2% it is shown in red.
const GAP_OK = 0.5
const GAP_BAD = 2

// Sub-cent / sub-0.005% differences print as zero instead of "-0".
const pct = (value: number) => Math.abs(value) < 0.005 ? '0.00%' : `${value > 0 ? '+' : ''}${value.toFixed(2)}%`
const signedUsd = (value: number) => Math.abs(value) < 0.5 ? '0$' : `${value > 0 ? '+' : '-'}${usd(Math.abs(value))}`

function gapTone(value: number) {
  const abs = Math.abs(value)
  return abs <= GAP_OK ? 'positive' : abs <= GAP_BAD ? 'leverage--elevated' : 'negative'
}

// Kraken LIVE: each part's effect on live equity against expected (positive = in favour
// of live), totals since the study started. What is left is quantity rounding,
// close-versus-open sizing and skipped markets.
function explainedParts(row: LiveVsExpectedRow, difference: number) {
  const parts = [
    { label: 'Fees (expected − real)', value: (row.expectedFees ?? 0) - (row.liveFees ?? 0) },
    { label: 'Funding', value: row.liveFunding ?? 0 },
    { label: 'Interest', value: row.liveInterest ?? 0 },
    { label: 'Fill prices vs Binance open', value: -(row.slippage ?? 0) },
  ]
  const explained = parts.reduce((sum, part) => sum + part.value, 0)
  return [...parts, { label: 'Other (rounding, sizing, skipped markets)', value: difference - explained }]
}

export function LiveVsExpectedPage() {
  const { data, error, retry } = useDashboardResource('getLiveVsExpected')
  const [range, setRange] = useState<TimeRange>(DEFAULT_DAILY_RANGE)
  const [limit, setLimit] = useState(PAGE)

  const rows = useMemo(() => (data?.paperComparison?.rows ?? []).map(row => ({
    ...row,
    label: row.date,
    equity: row.liveEquity,
    expected: row.expectedEquity,
    difference: row.liveEquity - row.expectedEquity,
    gap: row.expectedEquity !== 0 ? (row.liveEquity / row.expectedEquity - 1) * 100 : 0,
  })), [data])

  if (error) return <DataLoadError title="Live vs expected unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading live vs expected…</div>

  const comparison = data.paperComparison
  const positions = comparison?.positions ?? []
  if (!comparison || rows.length === 0) return <>
    <div className="page-heading"><h1>Live vs Expected</h1></div>
    <div className="overview-unavailable"><strong>No comparison yet</strong><span>{comparison?.note || 'Waiting for the first completed day with an expected value.'}</span></div>
  </>

  const latest = rows[rows.length - 1]
  const largest = rows.reduce((worst, row) => Math.abs(row.gap) > Math.abs(worst.gap) ? row : worst, rows[0])
  const matching = rows.filter(row => Math.abs(row.gap) <= GAP_OK).length
  const showSignals = rows.some(row => row.signalDifferences > 0)
  const venueCosts = latest.liveFees !== undefined
  const skippedNow = latest.skipped ?? []
  const days = rows.map(row => chartDate(row.label)).filter(Number.isFinite)
  const visible = dailyWindow(range, Math.min(...days), Math.max(...days))
  const shared = { ...visible, onSelectRange: (from: number, to: number) => setRange({ kind: 'custom', from, to }), onReset: () => setRange(DEFAULT_DAILY_RANGE) }
  const recent = [...rows].reverse()

  return <>
    <div className="page-heading">
      <h1>Live vs Expected</h1>
      <TimeRangePicker presets={DAILY_PRESETS} value={range} onChange={setRange} precision="date"/>
    </div>

    {comparison.status === 'STALE' && <div className="manual-banner manual-banner--bad">The comparison has not been refreshed recently; values below are the last ones received.</div>}

    <section className="overview-stats">
      <div className="overview-stat"><span>Equity</span><strong>{usd(latest.equity)}</strong></div>
      <div className="overview-stat"><span>Expected</span><strong>{usd(latest.expected)}</strong></div>
      <div className="overview-stat"><span>Difference</span><strong className={gapTone(latest.gap)}>{signedUsd(latest.difference)}</strong></div>
      <div className="overview-stat"><span>Difference %</span><strong className={gapTone(latest.gap)}>{pct(latest.gap)}</strong></div>
      <div className="overview-stat"><span>Largest Gap</span><strong className={gapTone(largest.gap)}>{pct(largest.gap)}</strong><small>{largest.label}</small></div>
      <div className="overview-stat"><span>Days Matching</span><strong>{matching} / {rows.length}</strong></div>
    </section>

    <section className="risk-charts">
      <Panel title="Equity vs Expected"><LineChart data={rows} lines={[EXPECTED, EQUITY]} compact shared={shared}/></Panel>
      <Panel title="Difference (%)"><LineChart data={rows} lines={[GAP]} formatValue={pct} compact shared={shared}/></Panel>
    </section>

    {venueCosts && <Panel title={`Where the difference comes from (since the start, ${latest.label})`}>
      <div className="table-wrap"><table>
        <thead><tr><th>Part</th><th>Effect on equity</th></tr></thead>
        <tbody>
          {explainedParts(latest, latest.difference).map(part => <tr key={part.label}>
            <td>{part.label}</td>
            <td className={part.value < -0.5 ? 'negative' : part.value > 0.5 ? 'positive' : ''}>{signedUsd(part.value)}</td>
          </tr>)}
          <tr><td><strong>Difference</strong></td><td className={gapTone(latest.gap)}><strong>{signedUsd(latest.difference)}</strong></td></tr>
          {skippedNow.length > 0 && <tr><td>Skipped at Kraken (last cycle)</td><td>{skippedNow.join(', ')}</td></tr>}
          {(latest.capitalFlows ?? 0) !== 0 && <tr><td>Deposits − withdrawals (in both runs)</td><td>{signedUsd(latest.capitalFlows ?? 0)}</td></tr>}
        </tbody>
      </table></div>
    </Panel>}

    {positions.length > 0 && <Panel title="Positions vs Expected">
      <div className="table-wrap"><table className="wide-table">
        <thead><tr><th>Asset</th><th>Quantity</th><th>Expected Qty</th><th>Value</th><th>Expected Value</th><th>Difference</th></tr></thead>
        <tbody>{positions.map(row => {
          const difference = row.liveUsd - row.expectedUsd
          const off = Math.abs(difference) > Math.max(1, Math.abs(row.expectedUsd) * GAP_OK / 100)
          return <tr key={row.asset}>
            <td><strong>{row.asset}</strong></td>
            <td>{row.liveQuantity.toPrecision(6)}</td>
            <td>{row.expectedQuantity.toPrecision(6)}</td>
            <td>{usd(row.liveUsd)}</td>
            <td>{usd(row.expectedUsd)}</td>
            <td className={off ? 'negative' : 'positive'}>{signedUsd(difference)}</td>
          </tr>
        })}</tbody>
      </table></div>
    </Panel>}

    <Panel title="Daily Comparison">
      <div className="table-wrap"><table className="wide-table">
        <thead><tr><th>Date</th><th>Equity</th><th>Expected</th><th>Difference</th><th>Difference %</th>{showSignals && <th>Signal Mismatches</th>}
          {venueCosts && <><th>Fees</th><th>Expected Fees</th><th>Funding + Interest</th><th>Slippage</th><th>Skipped</th></>}</tr></thead>
        <tbody>{recent.slice(0, limit).map(row => <tr key={row.label}>
          <td>{row.label}</td>
          <td>{usd(row.equity)}</td>
          <td>{usd(row.expected)}</td>
          <td className={gapTone(row.gap)}>{signedUsd(row.difference)}</td>
          <td className={gapTone(row.gap)}>{pct(row.gap)}</td>
          {showSignals && <td className={row.signalDifferences > 0 ? 'negative' : ''}>{row.signalDifferences}</td>}
          {venueCosts && <>
            <td>{row.liveFees !== undefined ? usd(row.liveFees) : '—'}</td>
            <td>{row.expectedFees !== undefined ? usd(row.expectedFees) : '—'}</td>
            <td>{row.liveFunding !== undefined ? signedUsd((row.liveFunding ?? 0) + (row.liveInterest ?? 0)) : '—'}</td>
            <td>{row.slippage !== undefined ? signedUsd(row.slippage) : '—'}</td>
            <td>{row.skipped?.length ? row.skipped.join(', ') : '—'}</td>
          </>}
        </tr>)}</tbody>
      </table></div>
      {recent.length > limit && <div className="table-more"><span className="muted">{limit} of {recent.length}</span><button className="ghost-button" onClick={() => setLimit(value => value + PAGE)}>Show more</button></div>}
    </Panel>
  </>
}
