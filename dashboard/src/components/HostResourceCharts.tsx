import { useEffect, useMemo, useState } from 'react'
import { useDashboardDataSource } from '../providers/DashboardDataSourceProvider'
import type { HostHistoryData, HostHistoryPoint, HostHistoryQuery, HostHistoryRange } from '../types/dashboard'
import { Panel } from './Panel'
import { TimeSeriesChart } from './TimeSeriesChart'
import type { ChartSeries, TooltipRow } from './TimeSeriesChart'
import { TimeRangePicker } from './TimeRangePicker'
import type { TimeRange } from './TimeRangePicker'

// Host CPU / RAM / disk over time, Grafana style: one line per chart (host total) and, for
// CPU and RAM, "By container" shows one colored line per container in the same chart.
// All three are percentages: RAM and disk of the host's total (shown in the panel title),
// a container's RAM of the host's total RAM.
// Time range: presets ending now, an explicit From/To, or a drag-selection on any chart.

const PRESETS: { value: HostHistoryRange; label: string; title: string; ms: number }[] = [
  { value: '1h', label: '1H', title: 'Last 1 hour', ms: 3_600_000 },
  { value: '6h', label: '6H', title: 'Last 6 hours', ms: 6 * 3_600_000 },
  { value: '24h', label: '24H', title: 'Last 24 hours', ms: 24 * 3_600_000 },
  { value: '7d', label: '7D', title: 'Last 7 days', ms: 7 * 24 * 3_600_000 },
]
const DEFAULT_RANGE: TimeRange = { kind: 'preset', preset: '1h' }
// One line per container; only beyond this many are the lightest summed as "Other containers".
const MAX_CONTAINER_LINES = 30
// Seven validated hues x five dash patterns identify up to 35 lines (never extra hues).
const DASH_PATTERNS = [undefined, '6 4', '2 3', '10 3 2 3', '1 5']
const REFRESH_MS = 30_000

const formatPct = (value: number) => `${value.toFixed(Math.abs(value) < 1 ? 2 : Math.abs(value) < 10 ? 1 : 0)}%`
const formatMiB = (value: number) => Math.abs(value) >= 1024 ? `${(value / 1024).toFixed(2)} GiB` : `${value.toFixed(0)} MiB`
const formatGiB = (value: number) => `${value.toFixed(1)} GiB`

function queryFor(range: TimeRange): HostHistoryQuery {
  if (range.kind === 'custom')
    return { from: new Date(range.from).toISOString(), to: new Date(range.to).toISOString() }
  return { range: range.preset as HostHistoryRange }
}

function useHostHistory(range: TimeRange) {
  const dataSource = useDashboardDataSource()
  const [data, setData] = useState<HostHistoryData | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const key = JSON.stringify(range)
  useEffect(() => {
    let active = true
    const query = queryFor(JSON.parse(key) as TimeRange)
    const load = () => dataSource.getHostHistory(query)
      .then(result => { if (active) { setData(result); setError(null) } })
      .catch(reason => { if (active) setError(reason instanceof Error ? reason : new Error(String(reason))) })
    load()
    // Presets end "now" and refresh; an explicit window is fixed, like Grafana.
    const timer = 'range' in query ? window.setInterval(load, REFRESH_MS) : undefined
    return () => { active = false; if (timer !== undefined) window.clearInterval(timer) }
  }, [dataSource, key])
  return { data, error }
}

// One line per container; beyond MAX_CONTAINER_LINES the lightest are summed.
function containerLines(points: HostHistoryPoint[], containers: string[], perContainer: (point: HostHistoryPoint) => Record<string, number>) {
  const average = (name: string) => points.reduce((sum, point) => sum + (perContainer(point)[name] ?? 0), 0) / Math.max(1, points.length)
  const ranked = [...containers].sort((a, b) => average(b) - average(a))
  // Style follows the container name (alphabetical order), not its rank.
  const named = ranked.slice(0, MAX_CONTAINER_LINES).sort()
  const folded = ranked.slice(MAX_CONTAINER_LINES).sort()
  const series: ChartSeries[] = named.map((name, index) => ({
    key: name, label: name, color: `var(--series-${(index % 7) + 1})`, dash: DASH_PATTERNS[Math.floor(index / 7) % DASH_PATTERNS.length],
    values: points.map(point => perContainer(point)[name] ?? null),
  }))
  if (folded.length) series.push({
    key: 'other', label: `Other containers (${folded.length})`, color: 'var(--series-neutral)',
    values: points.map(point => {
      const present = folded.filter(name => perContainer(point)[name] !== undefined)
      return present.length ? present.reduce((sum, name) => sum + perContainer(point)[name], 0) : null
    }),
  })
  // Individual values of the summed containers, for the tooltip.
  const foldedValues = (index: number) => folded
    .filter(name => perContainer(points[index])[name] !== undefined)
    .map(name => ({ name, value: perContainer(points[index])[name] }))
  return { series, folded, foldedValues }
}

interface ResourceChartProps {
  title: string
  subtitle: string
  window: { from: number; to: number }
  times: number[]
  maxGapMs: number
  overall: ChartSeries[]
  formatValue: (value: number) => string
  byContainer?: ReturnType<typeof containerLines>
  breakdownNote?: string
  onSelectRange: (from: number, to: number) => void
  onReset: () => void
  split: boolean                 // Overall or By container; kept by the parent across reloads.
  onSplit: (split: boolean) => void
}

function ResourceChart({ title, subtitle, window: visible, times, maxGapMs, overall, formatValue, byContainer, breakdownNote, onSelectRange, onReset, split, onSplit }: ResourceChartProps) {
  const showContainers = split && byContainer !== undefined
  const toggle = byContainer
    ? <div className="range" role="group" aria-label={`${title} view`}>
        <button className={!split ? 'active' : ''} aria-pressed={!split} onClick={() => onSplit(false)}>Overall</button>
        <button className={split ? 'active' : ''} aria-pressed={split} onClick={() => onSplit(true)}>By container</button>
      </div>
    : breakdownNote && <small>{breakdownNote}</small>
  // Each resource gets its own panel so the three charts read as separate blocks.
  return <Panel title={subtitle ? `${title} · ${subtitle}` : title} right={toggle} className="resource-chart">
    <TimeSeriesChart
      times={times}
      series={showContainers ? byContainer.series : overall}
      from={visible.from}
      to={visible.to}
      formatValue={formatValue}
      timeLabels="datetime"
      yFloor={0}
      maxGapMs={maxGapMs}
      height={230}
      ariaLabel={`${title} over time`}
      onSelectRange={onSelectRange}
      onReset={onReset}
      sortTooltipByValue={showContainers}
      extraTooltipRows={showContainers && byContainer.folded.length
        ? (index): TooltipRow[] => byContainer.foldedValues(index).map(row => ({ label: `· ${row.name}`, value: formatValue(row.value) }))
        : undefined}/>
  </Panel>
}

export function HostResourceCharts() {
  const [range, setRange] = useState<TimeRange>(DEFAULT_RANGE)
  const { data, error } = useHostHistory(range)
  // Overall / By container per chart; survives zooming and range changes.
  const [split, setSplit] = useState<Record<string, boolean>>({})
  const splitProps = (title: string) => ({ split: split[title] ?? false, onSplit: (value: boolean) => setSplit(current => ({ ...current, [title]: value })) })

  const view = useMemo(() => {
    if (!data || !data.available) return null
    const points = data.points
    const times = points.map(point => Date.parse(point.t))
    const pick = (field: 'cpuPct' | 'ramUsedMiB' | 'ramTotalMiB' | 'diskUsedGiB' | 'diskTotalGiB') => points.map(point => point[field])
    const latest = (values: (number | null)[]) => [...values].reverse().find(value => value !== null) ?? null
    const percent = (used: number | null, total: number | null) => used !== null && total ? 100 * used / total : null
    // Container RAM as a share of the host RAM at the same observation.
    const containerRamPct = (point: HostHistoryPoint) => Object.fromEntries(Object.entries(point.containerRamMiB)
      .flatMap(([name, used]) => point.ramTotalMiB ? [[name, 100 * used / point.ramTotalMiB]] : []))
    return {
      times,
      maxGapMs: Math.max(data.bucketSeconds * 1000 * 3.5, 90_000),
      cpu: [{ key: 'cpu', label: 'Host CPU', color: 'var(--series-1)', values: pick('cpuPct') }],
      ram: [{ key: 'ram', label: 'Used RAM', color: 'var(--series-1)', values: points.map(point => percent(point.ramUsedMiB, point.ramTotalMiB)) }],
      disk: [{ key: 'disk', label: 'Used disk', color: 'var(--series-1)', values: points.map(point => percent(point.diskUsedGiB, point.diskTotalGiB)) }],
      ramTotal: latest(pick('ramTotalMiB')),
      diskTotal: latest(pick('diskTotalGiB')),
      cpuByContainer: containerLines(points, data.containers, point => point.containerCpuPct),
      ramByContainer: containerLines(points, data.containers, containerRamPct),
    }
  }, [data])

  // Visible window: presets end now; custom ranges are exact.
  const now = Date.now()
  const visible = range.kind === 'custom' ? { from: range.from, to: range.to }
    : { from: now - (PRESETS.find(item => item.value === range.preset)?.ms ?? PRESETS[1].ms), to: now }
  const select = (from: number, to: number) => setRange({ kind: 'custom', from, to })
  const reset = () => setRange(DEFAULT_RANGE)
  const shared = { window: visible, onSelectRange: select, onReset: reset }

  return <div className="resource-charts">
    <div className="chart-toolbar">
      <TimeRangePicker presets={PRESETS} value={range} onChange={setRange} precision="datetime"/>
      {data?.stale && <small className="warning">No new observation since {data.newestAt}</small>}
    </div>
    {error && <div className="muted">Resource history unavailable: {error.message}</div>}
    {!error && !data && <div className="muted">Loading resource history…</div>}
    {data && !view && <div className="muted">{data.reason ?? 'No host observations in this range'}</div>}
    {view && <>
      <ResourceChart title="CPU" subtitle="" times={view.times} maxGapMs={view.maxGapMs}
        overall={view.cpu} formatValue={formatPct} byContainer={view.cpuByContainer} {...shared} {...splitProps('CPU')}/>
      <ResourceChart title="RAM" subtitle={view.ramTotal !== null ? `${formatMiB(view.ramTotal)} installed` : ''}
        times={view.times} maxGapMs={view.maxGapMs} overall={view.ram} formatValue={formatPct} byContainer={view.ramByContainer} {...shared} {...splitProps('RAM')}/>
      <ResourceChart title="Disk" subtitle={view.diskTotal !== null ? `${formatGiB(view.diskTotal)} capacity` : ''}
        times={view.times} maxGapMs={view.maxGapMs} overall={view.disk} formatValue={formatPct}
        {...shared} {...splitProps('Disk')}/>
    </>}
  </div>
}
