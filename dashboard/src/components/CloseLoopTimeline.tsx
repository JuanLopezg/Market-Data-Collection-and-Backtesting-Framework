import { useState } from 'react'
import type { PointerEvent as ReactPointerEvent } from 'react'
import { fullTimeLabel, tickLabel, timeAxis, useChartWidth } from './TimeSeriesChart'

// Grafana-style state timeline for the close loop: one band over time, green while local
// state and venue state reconcile (CLEAN), red otherwise. Same time axis, drag-to-zoom
// and double-click reset as TimeSeriesChart, so it lines up under the equity chart.

const DAY = 86_400_000
// No value axis here, so the band keeps the same margin on both sides.
const PAD = { left: 8, right: 8 }
const BAND = 34
const HEIGHT = BAND + 30

interface CloseLoopTimelineProps {
  times: number[]                // Unix ms, ascending; one entry per observation.
  clean: (boolean | null)[]      // null = no observation that day.
  from: number
  to: number
  onSelectRange?: (from: number, to: number) => void
  onReset?: () => void
}

export function CloseLoopTimeline({ times, clean, from, to, onSelectRange, onReset }: CloseLoopTimelineProps) {
  const { ref, width } = useChartWidth()
  const [hover, setHover] = useState<{ index: number; px: number } | null>(null)
  const [selection, setSelection] = useState<{ start: number; end: number } | null>(null)

  const plotWidth = width - PAD.left - PAD.right
  const span = Math.max(to - from, 1)
  const x = (time: number) => PAD.left + (time - from) / span * plotWidth
  const timeAt = (px: number) => from + (px - PAD.left) / plotWidth * span
  const clampX = (px: number) => Math.min(Math.max(px, PAD.left), width - PAD.right)
  const { step, ticks } = timeAxis(from, to, plotWidth)

  // Each observation colours the band until the next one (at most one day); equal
  // consecutive days merge into one run so the band has no hairline gaps.
  const days = times.map((time, index) => ({ index, start: time, end: Math.min(times[index + 1] ?? time + DAY, time + DAY), state: clean[index] }))
  const runs: { index: number; start: number; end: number; state: boolean | null }[] = []
  for (const day of days) {
    const last = runs[runs.length - 1]
    if (last && last.state === day.state && last.end >= day.start) last.end = day.end
    else runs.push({ ...day })
  }
  const segments = runs
    .map(run => ({ ...run, start: Math.max(x(run.start), PAD.left), end: Math.min(x(run.end), width - PAD.right) }))
    .filter(segment => segment.state !== null && segment.end > segment.start)
  // Hover reports the day under the pointer, not the run.
  const dayAt = (px: number) => days.find(day => day.state !== null && px >= x(day.start) && px <= x(day.end))

  const locate = (event: ReactPointerEvent<SVGSVGElement>) => event.clientX - event.currentTarget.getBoundingClientRect().left

  function pointerMove(event: ReactPointerEvent<SVGSVGElement>) {
    const px = locate(event)
    if (selection) setSelection({ ...selection, end: clampX(px) })
    const found = px >= PAD.left && px <= width - PAD.right ? dayAt(px) : undefined
    setHover(found ? { index: found.index, px } : null)
  }

  function pointerUp() {
    if (selection && Math.abs(selection.end - selection.start) >= 6 && onSelectRange)
      onSelectRange(Math.round(timeAt(Math.min(selection.start, selection.end))), Math.round(timeAt(Math.max(selection.start, selection.end))))
    setSelection(null)
  }

  return <div className="close-loop" ref={ref}>
    <svg width={width} height={HEIGHT} role="img" aria-label="Close loop state over time: green when reconciled, red otherwise"
      onPointerMove={pointerMove} onPointerLeave={() => setHover(null)}
      onPointerDown={event => {
        if (event.button !== 0 || !onSelectRange) return
        const px = clampX(locate(event))
        setSelection({ start: px, end: px })
        event.currentTarget.setPointerCapture(event.pointerId)
      }}
      onPointerUp={pointerUp} onPointerCancel={() => setSelection(null)}
      onDoubleClick={() => { setSelection(null); onReset?.() }}>
      <rect x={PAD.left} y={0} width={plotWidth} height={BAND} className="close-loop__empty"/>
      {segments.map(segment => <rect key={segment.index} x={segment.start} y={0} width={segment.end - segment.start} height={BAND}
        className={segment.state ? 'close-loop__clean' : 'close-loop__broken'} shapeRendering="crispEdges"/>)}
      {selection && <rect x={Math.min(selection.start, selection.end)} y={0} width={Math.abs(selection.end - selection.start)} height={BAND} className="ts-chart__selection"/>}
      {ticks.map(tick => <text key={tick} x={x(tick)} y={HEIGHT - 9} textAnchor="middle" className="ts-chart__tick">{tickLabel(tick, step)}</text>)}
    </svg>
    {hover && !selection && <div className="ts-chart__tooltip close-loop__tooltip" role="status"
      style={{ left: hover.px > width * 0.6 ? undefined : hover.px + 12, right: hover.px > width * 0.6 ? width - hover.px + 12 : undefined }}>
      <span className={clean[hover.index] ? 'close-loop__dot close-loop__dot--clean' : 'close-loop__dot close-loop__dot--broken'}/>
      <strong>{fullTimeLabel(times[hover.index], 'date')}</strong>
    </div>}
  </div>
}

