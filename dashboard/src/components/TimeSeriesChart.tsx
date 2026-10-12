import { useEffect, useId, useMemo, useRef, useState } from 'react'
import type { PointerEvent as ReactPointerEvent } from 'react'

// Time-series line chart in the Grafana style, shared by the equity curve and the host
// resource charts.
//
// - The visible window [from, to] is chosen outside the chart (TimeRangePicker).
// - Dragging across the plot selects a time span; onSelectRange receives it (zoom in).
// - Double-click calls onReset (back to the default range).
// - The value axis fits the visible data; the crosshair tooltip lists every series.
// All times are UTC. Missing observations are drawn as gaps, never interpolated.

export interface ChartSeries {
  key: string
  label: string
  color: string
  values: (number | null)[]
  dash?: string               // SVG stroke-dasharray; with color it identifies a series.
}

export interface TooltipRow {
  label: string
  value: string
}

interface TimeSeriesChartProps {
  times: number[]              // Unix ms, ascending.
  series: ChartSeries[]
  from: number                 // Visible window, Unix ms.
  to: number
  formatValue: (value: number) => string
  timeLabels: 'datetime' | 'date'
  yFloor?: number              // Fixed lower bound, e.g. 0 for resource usage.
  maxGapMs?: number            // A larger time gap breaks the line.
  height?: number
  ariaLabel: string
  onSelectRange?: (from: number, to: number) => void
  onReset?: () => void
  extraTooltipRows?: (index: number) => TooltipRow[]
  sortTooltipByValue?: boolean // Highest value first (e.g. per-container usage).
}

const MINUTE = 60_000
const HOUR = 60 * MINUTE
const DAY = 24 * HOUR
const TIME_STEPS = [MINUTE, 2 * MINUTE, 5 * MINUTE, 10 * MINUTE, 15 * MINUTE, 30 * MINUTE, HOUR, 2 * HOUR,
  3 * HOUR, 6 * HOUR, 12 * HOUR, DAY, 2 * DAY, 7 * DAY, 14 * DAY, 30 * DAY, 91 * DAY, 182 * DAY, 365 * DAY]
export const PAD = { left: 8, right: 74, top: 12, bottom: 28 }

function niceStep(span: number, target: number) {
  const raw = span / Math.max(1, target)
  const power = 10 ** Math.floor(Math.log10(raw))
  const unit = raw / power
  return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 2.5 ? 2.5 : unit <= 5 ? 5 : 10) * power
}

const monthDay = new Intl.DateTimeFormat('en-US', { month: 'short', day: 'numeric', timeZone: 'UTC' })
const monthYear = new Intl.DateTimeFormat('en-US', { month: 'short', year: 'numeric', timeZone: 'UTC' })

export function tickLabel(time: number, step: number) {
  const iso = new Date(time).toISOString()
  if (step >= 30 * DAY) return monthYear.format(time)
  if (step >= DAY || iso.slice(11, 16) === '00:00') return monthDay.format(time)
  return iso.slice(11, 16)
}

// Time ticks for a window: about one label per 110 px.
export function timeAxis(from: number, to: number, plotWidth: number) {
  const span = Math.max(to - from, MINUTE)
  const step = TIME_STEPS.find(item => span / item <= Math.max(2, Math.floor(plotWidth / 110))) ?? TIME_STEPS[TIME_STEPS.length - 1]
  const ticks: number[] = []
  for (let tick = Math.ceil(from / step) * step; tick <= to; tick += step) ticks.push(tick)
  return { step, ticks }
}

// Measured container width, so SVG text keeps its size.
export function useChartWidth() {
  const ref = useRef<HTMLDivElement>(null)
  const [width, setWidth] = useState(800)
  useEffect(() => {
    const element = ref.current
    if (!element) return
    const observer = new ResizeObserver(entries => setWidth(Math.max(320, Math.floor(entries[0].contentRect.width))))
    observer.observe(element)
    return () => observer.disconnect()
  }, [])
  return { ref, width }
}

function Swatch({ color, dash }: { color: string; dash?: string }) {
  return <svg className="ts-chart__swatch-line" width="22" height="8" aria-hidden="true">
    <line x1="1" x2="21" y1="4" y2="4" style={{ stroke: color }} strokeDasharray={dash} strokeWidth="2" strokeLinecap="round"/>
  </svg>
}

export function fullTimeLabel(time: number, mode: 'datetime' | 'date') {
  const iso = new Date(time).toISOString()
  return mode === 'date' ? iso.slice(0, 10) : `${iso.slice(0, 10)} ${iso.slice(11, 16)} UTC`
}

export function TimeSeriesChart(props: TimeSeriesChartProps) {
  const { times, series, from, to, formatValue, timeLabels, yFloor, height = 260, ariaLabel, onSelectRange, onReset, extraTooltipRows, sortTooltipByValue = false } = props
  const clipId = useId().replace(/:/g, '')
  const { ref: wrapRef, width } = useChartWidth()

  const medianStep = useMemo(() => {
    const steps = times.slice(1).map((time, index) => time - times[index]).sort((a, b) => a - b)
    return steps.length ? steps[Math.floor(steps.length / 2)] : HOUR
  }, [times])
  const maxGap = props.maxGapMs ?? Math.max(medianStep * 3.5, MINUTE)
  const span = Math.max(to - from, MINUTE)

  const plotWidth = width - PAD.left - PAD.right
  const plotHeight = height - PAD.top - PAD.bottom
  const x = (time: number) => PAD.left + (time - from) / span * plotWidth
  const timeAt = (px: number) => from + (px - PAD.left) / plotWidth * span

  // Grafana legend: click a series to show only it; click it again to show all.
  const [isolated, setIsolated] = useState<string | null>(null)
  const shown = useMemo(() => isolated && series.some(item => item.key === isolated)
    ? series.filter(item => item.key === isolated) : series, [isolated, series])

  // Value axis fits the observations inside the window.
  const yDomain = useMemo(() => {
    let low = Infinity, high = -Infinity
    times.forEach((time, index) => {
      if (time < from || time > to) return
      shown.forEach(item => {
        const value = item.values[index]
        if (value === null || value === undefined || !Number.isFinite(value)) return
        low = Math.min(low, value); high = Math.max(high, value)
      })
    })
    if (!Number.isFinite(low)) { low = yFloor ?? 0; high = (yFloor ?? 0) + 1 }
    if (yFloor !== undefined) low = yFloor
    const padding = Math.max((high - low) * 0.08, Math.abs(high) * 0.0005, yFloor !== undefined ? 0.5 : 0.01)
    return { low: yFloor !== undefined ? low : low - padding, high: high + padding }
  }, [times, from, to, shown, yFloor])
  const y = (value: number) => PAD.top + (yDomain.high - value) / (yDomain.high - yDomain.low) * plotHeight

  const [hover, setHover] = useState<{ index: number; px: number; py: number } | null>(null)
  // New data (e.g. after a zoom re-fetch) invalidates the hovered index.
  useEffect(() => setHover(null), [times])
  const [selection, setSelection] = useState<{ start: number; end: number } | null>(null)

  function locate(event: ReactPointerEvent<SVGSVGElement>) {
    const rect = event.currentTarget.getBoundingClientRect()
    return { px: event.clientX - rect.left, py: event.clientY - rect.top }
  }
  const clampX = (px: number) => Math.min(Math.max(px, PAD.left), width - PAD.right)

  function pointerMove(event: ReactPointerEvent<SVGSVGElement>) {
    const { px, py } = locate(event)
    if (selection) setSelection({ ...selection, end: clampX(px) })
    if (px < PAD.left || px > width - PAD.right || py < PAD.top || py > height - PAD.bottom) { setHover(null); return }
    const target = timeAt(px)
    let nearest = -1, distance = Infinity
    times.forEach((time, index) => {
      if (time >= from && time <= to && Math.abs(time - target) < distance) { nearest = index; distance = Math.abs(time - target) }
    })
    setHover(nearest >= 0 ? { index: nearest, px, py } : null)
  }

  function pointerUp() {
    if (selection && Math.abs(selection.end - selection.start) >= 6 && onSelectRange) {
      const a = timeAt(Math.min(selection.start, selection.end)), b = timeAt(Math.max(selection.start, selection.end))
      onSelectRange(Math.round(a), Math.round(b))
    }
    setSelection(null)
  }

  // Line paths, split at gaps and missing values.
  function paths(values: (number | null)[]) {
    const result: string[] = []
    let current = ''
    times.forEach((time, index) => {
      const value = values[index]
      const gap = index > 0 && time - times[index - 1] > maxGap
      if (value === null || value === undefined || !Number.isFinite(value) || gap) {
        if (current) result.push(current)
        current = ''
      }
      if (value !== null && value !== undefined && Number.isFinite(value))
        current += `${current ? 'L' : 'M'}${x(time).toFixed(1)},${y(value).toFixed(1)}`
    })
    if (current) result.push(current)
    return result
  }

  const { step: timeStep, ticks: timeTicks } = timeAxis(from, to, plotWidth)
  const valueStep = niceStep(yDomain.high - yDomain.low, 4)
  const valueTicks: number[] = []
  for (let tick = Math.ceil(yDomain.low / valueStep) * valueStep; tick <= yDomain.high; tick += valueStep) valueTicks.push(tick)

  const visible = times.some(time => time >= from && time <= to)
  const selected = hover && hover.index < times.length ? hover.index : undefined
  const tooltipRows: { color?: string; dash?: string; label: string; value: string }[] = []
  if (selected !== undefined) {
    const valueOf = (item: ChartSeries) => item.values[selected] ?? null
    const ordered = sortTooltipByValue
      ? [...shown].sort((a, b) => (valueOf(b) ?? -Infinity) - (valueOf(a) ?? -Infinity))
      : shown
    ordered.forEach(item => {
      const value = valueOf(item)
      tooltipRows.push({ color: item.color, dash: item.dash, label: item.label, value: value === null ? '—' : formatValue(value) })
    })
    extraTooltipRows?.(selected).forEach(row => tooltipRows.push(row))
  }

  return <div className="ts-chart" ref={wrapRef}>
    <svg width={width} height={height} role="img" className="ts-chart__svg"
      aria-label={`${ariaLabel}. Drag across the chart to zoom into a time span; double-click to reset.`}
      onPointerMove={pointerMove} onPointerLeave={() => setHover(null)}
      onPointerDown={event => {
        if (event.button !== 0 || !onSelectRange) return
        const { px } = locate(event)
        setSelection({ start: clampX(px), end: clampX(px) })
        event.currentTarget.setPointerCapture(event.pointerId)
      }}
      onPointerUp={pointerUp} onPointerCancel={() => setSelection(null)}
      onDoubleClick={() => { setSelection(null); onReset?.() }}>
      <defs><clipPath id={clipId}><rect x={PAD.left} y={PAD.top} width={plotWidth} height={plotHeight}/></clipPath></defs>

      {valueTicks.map(tick => <g key={tick}>
        <line x1={PAD.left} x2={width - PAD.right} y1={y(tick)} y2={y(tick)} className="ts-chart__grid"/>
        <text x={width - PAD.right + 8} y={y(tick) + 4} className="ts-chart__tick">{formatValue(tick)}</text>
      </g>)}
      {timeTicks.map(tick => <text key={tick} x={x(tick)} y={height - 9} textAnchor="middle" className="ts-chart__tick">{tickLabel(tick, timeStep)}</text>)}

      <g clipPath={`url(#${clipId})`}>
        {shown.map(item => paths(item.values).map((d, k) => <path key={`${item.key}-${k}`} d={d} style={{ stroke: item.color }}
          strokeDasharray={item.dash} className="ts-chart__line"/>))}
        {selection && <rect x={Math.min(selection.start, selection.end)} y={PAD.top} width={Math.abs(selection.end - selection.start)} height={plotHeight} className="ts-chart__selection"/>}
        {hover && selected !== undefined && !selection && <>
          <line x1={x(times[selected])} x2={x(times[selected])} y1={PAD.top} y2={height - PAD.bottom} className="ts-chart__crosshair"/>
          {shown.map(item => {
            const value = item.values[selected]
            if (value === null || value === undefined) return null
            return <circle key={item.key} cx={x(times[selected])} cy={y(value)} r={4} style={{ fill: item.color }} className="ts-chart__marker"/>
          })}
        </>}
      </g>
    </svg>

    {!visible && <div className="ts-chart__empty">No observations in this time range</div>}

    {hover && selected !== undefined && !selection && <div className="ts-chart__tooltip" role="status"
      style={{ left: hover.px > width * 0.6 ? undefined : hover.px + 16, right: hover.px > width * 0.6 ? width - hover.px + 16 : undefined,
        top: hover.py <= height / 2 ? hover.py + 12 : undefined, bottom: hover.py > height / 2 ? height - hover.py + 12 : undefined }}>
      <strong>{fullTimeLabel(times[selected], timeLabels)}</strong>
      {tooltipRows.map((row, index) => <div key={index} className="ts-chart__tooltip-row">
        {row.color ? <Swatch color={row.color} dash={row.dash}/> : <span className="ts-chart__swatch-line"/>}
        <span>{row.label}</span><b>{row.value}</b>
      </div>)}
    </div>}

    {series.length > 1 && <div className="ts-chart__footer">
      <div className="ts-chart__legend">{series.map(item => {
        const dimmed = isolated !== null && isolated !== item.key
        return <button key={item.key} type="button" className={dimmed ? 'ts-chart__legend-item ts-chart__legend-item--dimmed' : 'ts-chart__legend-item'}
          aria-pressed={isolated === item.key} title="Click to show only this series; click again to show all"
          onClick={() => setIsolated(isolated === item.key ? null : item.key)}>
          <Swatch color={item.color} dash={item.dash}/>{item.label}</button>
      })}</div>
    </div>}
  </div>
}
