import { useId, useMemo, useRef, useState } from 'react'
import type { PointerEvent } from 'react'
import type { EquityPoint } from '../types/dashboard'

export const chartRanges = ['1D', '7D', '30D', '90D', '1Y', 'ALL'] as const
export type ChartRange = typeof chartRanges[number]
const day = 86_400_000
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
export function chartWindow(data: EquityPoint[], range: ChartRange) {
  const points = data.map(point => ({ ...point, time: chartDate(point.label) }))
    .filter(point => Number.isFinite(point.time) && Number.isFinite(point.equity) && Number.isFinite(point.benchmark))
    .sort((a, b) => a.time - b.time)
  if (!points.length || range === 'ALL') return points
  const days = range === '1Y' ? 365 : parseInt(range, 10)
  const end = points[points.length - 1].time
  return points.filter(point => point.time > end - days * day)
}
type Bounds = { xMin: number; xMax: number; yMin: number; yMax: number }
const money = (value: number) => new Intl.NumberFormat('en-US', { maximumFractionDigits: 2 }).format(value)
const date = (value: number) => new Date(value).toISOString().slice(0, 10)
export function LineChart({ data, compact = false, range = 'ALL' }: { data: EquityPoint[]; compact?: boolean; range?: ChartRange }) {
  const points = useMemo(() => chartWindow(data, range), [data, range])
  const width = compact ? 560 : 900, height = compact ? 300 : 350, left = 105, right = 22, top = 28, bottom = 48
  const plotWidth = width - left - right, plotHeight = height - top - bottom
  const clip = useId().replace(/:/g, '')
  const initial = useMemo<Bounds>(() => {
    const values = points.flatMap(point => [point.equity, point.benchmark])
    const low = values.reduce((minimum, value) => Math.min(minimum, value), values[0] ?? 0)
    const high = values.reduce((maximum, value) => Math.max(maximum, value), values[0] ?? 1)
    const padding = Math.max((high - low) * .08, Math.abs(high) * .001, 1)
    const first = points[0]?.time ?? 0, last = points[points.length - 1]?.time ?? day
    return { xMin: first === last ? first - day / 2 : first, xMax: first === last ? last + day / 2 : last, yMin: low - padding, yMax: high + padding }
  }, [points])
  // Reset navigation when the timeframe changes, but retain it during same-window refreshes.
  const signature = `${range}:${initial.xMin}:${initial.xMax}`
  const [view, setView] = useState<{ signature: string; bounds: Bounds } | null>(null)
  const bounds = view?.signature === signature ? view.bounds : initial
  const [hover, setHover] = useState<number | null>(null)
  const drag = useRef<{ x: number; y: number; bounds: Bounds } | null>(null)
  const x = (time: number) => left + (time - bounds.xMin) / (bounds.xMax - bounds.xMin) * plotWidth
  const y = (value: number) => top + (bounds.yMax - value) / (bounds.yMax - bounds.yMin) * plotHeight
  function zoom(axis: 'X' | 'Y', factor: number) {
    const next = { ...bounds }
    if (axis === 'X') {
      const middle = (bounds.xMin + bounds.xMax) / 2, half = Math.max(day / 48, (bounds.xMax - bounds.xMin) * factor / 2)
      next.xMin = middle - half; next.xMax = middle + half
    } else {
      const middle = (bounds.yMin + bounds.yMax) / 2, half = Math.max(.000001, (bounds.yMax - bounds.yMin) * factor / 2)
      next.yMin = middle - half; next.yMax = middle + half
    }
    setView({ signature, bounds: next }); setHover(null)
  }
  function location(event: PointerEvent<SVGSVGElement>) {
    const rect = event.currentTarget.getBoundingClientRect()
    return { x: (event.clientX - rect.left) * width / rect.width, y: (event.clientY - rect.top) * height / rect.height }
  }
  function move(event: PointerEvent<SVGSVGElement>) {
    const cursor = location(event)
    if (drag.current) {
      const start = drag.current
      const dx = (cursor.x - start.x) / plotWidth * (start.bounds.xMax - start.bounds.xMin)
      const dy = (cursor.y - start.y) / plotHeight * (start.bounds.yMax - start.bounds.yMin)
      setView({ signature, bounds: { xMin: start.bounds.xMin - dx, xMax: start.bounds.xMax - dx, yMin: start.bounds.yMin + dy, yMax: start.bounds.yMax + dy } })
      setHover(null); return
    }
    if (cursor.x < left || cursor.x > width - right || cursor.y < top || cursor.y > height - bottom) { setHover(null); return }
    const target = bounds.xMin + (cursor.x - left) / plotWidth * (bounds.xMax - bounds.xMin)
    let nearest = -1, distance = Infinity
    points.forEach((point, index) => {
      if (point.time >= bounds.xMin && point.time <= bounds.xMax && Math.abs(point.time - target) < distance) { nearest = index; distance = Math.abs(point.time - target) }
    })
    setHover(nearest >= 0 ? nearest : null)
  }
  const selected = hover === null ? undefined : points[hover]
  const demoDates = points.every(point => /^[A-Za-z]{3} \d{2}$/.test(point.label))
  const tickDate = (time: number) => demoDates
    ? new Intl.DateTimeFormat('en-US', { month: 'short', day: '2-digit', timeZone: 'UTC' }).format(time)
    : date(time)
  const make = (key: 'equity' | 'benchmark') => points.map(point => `${x(point.time)},${y(point[key])}`).join(' ')
  if (!points.length) return <div className="overview-unavailable"><strong>No history in this range</strong><span>Choose another timeframe or ALL. Only dated, finite source observations are shown.</span></div>
  return <div className="chart-wrap">
    <div className="chart-controls" aria-label="Chart navigation"><strong>Equity (USD)</strong>
      {(['X', 'Y'] as const).map(axis => <span key={axis}><button onClick={() => zoom(axis, .7)} aria-label={`Zoom in ${axis} axis`}>{axis} +</button><button onClick={() => zoom(axis, 1 / .7)} aria-label={`Zoom out ${axis} axis`}>{axis} −</button></span>)}
      <button onClick={() => { setView(null); setHover(null) }}>Reset view</button>
    </div>
    <svg viewBox={`0 0 ${width} ${height}`} className="line-chart line-chart--interactive" role="img" aria-label="Equity in USD; drag to pan both axes"
      onPointerMove={move} onPointerLeave={() => setHover(null)}
      onPointerDown={event => { if (event.button !== 0) return; const cursor = location(event); drag.current = { ...cursor, bounds }; event.currentTarget.setPointerCapture(event.pointerId) }}
      onPointerUp={() => { drag.current = null }} onPointerCancel={() => { drag.current = null }} onLostPointerCapture={() => { drag.current = null }}>
      <defs><clipPath id={clip}><rect x={left} y={top} width={plotWidth} height={plotHeight}/></clipPath></defs>
      {Array.from({ length: 5 }, (_, i) => {
        const yy = top + i * plotHeight / 4, value = bounds.yMax - i * (bounds.yMax - bounds.yMin) / 4
        return <g key={i}><line x1={left} x2={width - right} y1={yy} y2={yy} className="chart-grid"/><text x={left - 10} y={yy + 4} textAnchor="end" className="chart-tick">{money(value)}</text></g>
      })}
      {[0, .5, 1].map(fraction => <text key={fraction} x={left + fraction * plotWidth} y={height - 16} textAnchor={fraction === 0 ? 'start' : fraction === 1 ? 'end' : 'middle'} className="chart-tick">{tickDate(bounds.xMin + fraction * (bounds.xMax - bounds.xMin))}</text>)}
      <g clipPath={`url(#${clip})`}><polyline points={make('benchmark')} className="chart-line chart-line--muted"/><polyline points={make('equity')} className="chart-line chart-line--primary"/>
        {points.length === 1 && <circle cx={x(points[0].time)} cy={y(points[0].equity)} r={4} fill="var(--blue)"/>}
        {selected && <><line x1={x(selected.time)} x2={x(selected.time)} y1={top} y2={height - bottom} className="chart-crosshair"/><circle cx={x(selected.time)} cy={y(selected.equity)} r={4} fill="var(--blue)"/></>}
      </g>
    </svg>
    <div className="chart-readout" aria-live="polite">{selected ? `${selected.label} · Equity: ${money(selected.equity)} USD · Reference: ${money(selected.benchmark)} USD` : `${points.length} observations · ${points[0].label} – ${points[points.length - 1].label} · Hover for values; drag to pan X and Y.`}</div>
    <div className="chart-legend"><span>━━ Equity</span><span>┄┄ Reference (source-provided)</span><span>Dates in UTC · {range}</span></div>
  </div>
}
