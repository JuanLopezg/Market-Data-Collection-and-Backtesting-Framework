import { useMemo, useState } from 'react'
import { TimeSeriesChart } from './TimeSeriesChart'
import { TimeRangePicker } from './TimeRangePicker'
import type { TimeRange } from './TimeRangePicker'

// Daily USD lines (equity, balance, expected...), Grafana style: preset or From/To time
// range, drag across the chart to zoom into a span, double-click to return to ALL.

const DAY = 86_400_000
export const DAILY_PRESETS = [
  { value: '7D', label: '7D', title: 'Last 7 days', days: 7 },
  { value: '1M', label: '1M', title: 'Last 30 days', days: 30 },
  { value: '6M', label: '6M', title: 'Last 6 months', days: 182 },
  { value: '1Y', label: '1Y', title: 'Last year', days: 365 },
  { value: 'ALL', label: 'ALL', title: 'All time', days: null },
]
export const DEFAULT_DAILY_RANGE: TimeRange = { kind: 'preset', preset: 'ALL' }

// Visible window for daily data between first and last observation.
export function dailyWindow(range: TimeRange, first: number, last: number) {
  let from = first, to = last
  if (range.kind === 'custom') {
    from = range.from; to = range.to
  } else {
    const days = DAILY_PRESETS.find(item => item.value === range.preset)?.days ?? null
    if (days !== null) from = Math.max(first, last - days * DAY)
  }
  if (to - from < DAY) { from -= DAY / 2; to += DAY / 2 } // A single observation still gets a visible window.
  return { from, to }
}

export function chartDate(label: string): number {
  const full = /^(\d{4})-?(\d{2})-?(\d{2})$/.exec(label)
  if (full) {
    const time = Date.UTC(+full[1], +full[2] - 1, +full[3])
    const parsed = new Date(time)
    return parsed.getUTCFullYear() === +full[1] && parsed.getUTCMonth() === +full[2] - 1 && parsed.getUTCDate() === +full[3] ? time : NaN
  }
  // Yearless month/day labels are only used by demo fixtures.
  if (/^[A-Za-z]{3} \d{2}$/.test(label)) return Date.parse(`${label} 2000 00:00:00 GMT`)
  return Date.parse(label)
}

// "300,000$": the unit travels with every number, so no separate USD captions.
export const usd = (value: number) =>
  `${new Intl.NumberFormat('en-US', { maximumFractionDigits: Math.abs(value) >= 1000 ? 0 : 2 }).format(value)}$`

// Any object with a date label; the lines name its numeric fields.
// Providers send amounts as "$112302.59" / "+$0.00"; show them like the charts ("112,303$").
export function displayAmount(value: string) {
  const match = /^([+-]?)\$(-?[\d,]+(?:\.\d+)?)$/.exec(value.trim())
  if (!match) return value
  const amount = Number(match[2].replace(/,/g, ''))
  return Number.isFinite(amount) ? `${match[1]}${usd(amount)}` : value
}

export interface DailyPoint {
  label: string
}

export interface DailyLine {
  key: string                    // Field of each point.
  label: string
  color: string
}

interface LineChartProps<P extends DailyPoint> {
  data: P[]
  lines: DailyLine[]
  compact?: boolean
  formatValue?: (value: number) => string   // axis/tooltip values; USD by default
  // Shared window from the page (one picker for several charts); omit for a standalone chart.
  shared?: { from: number; to: number; onSelectRange: (from: number, to: number) => void; onReset: () => void }
}

export function LineChart<P extends DailyPoint>({ data, lines, compact = false, formatValue = usd, shared }: LineChartProps<P>) {
  const [range, setRange] = useState<TimeRange>(DEFAULT_DAILY_RANGE)
  const points = useMemo(() => data
    .map(point => ({ point, time: chartDate(point.label) }))
    .filter(row => Number.isFinite(row.time))
    .sort((a, b) => a.time - b.time), [data])

  if (!points.length) return <div className="overview-unavailable"><strong>No history yet</strong></div>

  const { from, to } = shared ?? dailyWindow(range, points[0].time, points[points.length - 1].time)

  const value = (point: P, key: string) => {
    const raw = (point as unknown as Record<string, unknown>)[key]
    return typeof raw === 'number' && Number.isFinite(raw) ? raw : null
  }
  const series = lines
    .map(line => ({ ...line, values: points.map(row => value(row.point, line.key)) }))
    .filter(line => line.values.some(item => item !== null))
  const times = points.map(row => row.time)
  const onSelectRange = shared?.onSelectRange ?? ((a: number, b: number) => setRange({ kind: 'custom', from: a, to: b }))
  const onReset = shared?.onReset ?? (() => setRange(DEFAULT_DAILY_RANGE))

  return <div>
    {!shared && <div className="chart-toolbar">
      <TimeRangePicker presets={DAILY_PRESETS} value={range} onChange={setRange} precision="date" presetsOnly={compact}/>
    </div>}
    <TimeSeriesChart
      times={times}
      series={series}
      from={from}
      to={to}
      formatValue={formatValue}
      timeLabels="date"
      maxGapMs={10 * DAY}
      height={compact ? 260 : 320}
      ariaLabel={lines.map(line => line.label).join(' and ')}
      onSelectRange={onSelectRange}
      onReset={onReset}/>
  </div>
}
