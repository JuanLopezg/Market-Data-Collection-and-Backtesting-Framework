import type { BehaviourTrendPoint } from '../types/dashboard'

function pointsFor(values: number[], width: number, height: number, pad: number, min: number, span: number) {
  if (values.length === 1) return `${width / 2},${height / 2}`
  return values.map((value, index) => {
    const x = pad + (index / (values.length - 1)) * (width - pad * 2)
    const y = height - pad - ((value - min) / span) * (height - pad * 2)
    return `${x},${y}`
  }).join(' ')
}

function bandPath(data: BehaviourTrendPoint[], width: number, height: number, pad: number, min: number, span: number) {
  if (!data.length) return ''
  const x = (index: number) => pad + (index / Math.max(1, data.length - 1)) * (width - pad * 2)
  const y = (value: number) => height - pad - ((value - min) / span) * (height - pad * 2)
  const upper = data.map((point, index) => `${x(index)},${y(point.upper)}`)
  const lower = [...data].reverse().map((point, reverseIndex) => {
    const index = data.length - 1 - reverseIndex
    return `${x(index)},${y(point.lower)}`
  })
  return `M ${upper.join(' L ')} L ${lower.join(' L ')} Z`
}

export function BaselineTrendChart({ data, compact = false }: { data: BehaviourTrendPoint[]; compact?: boolean }) {
  const width = compact ? 420 : 900
  const height = compact ? 126 : 280
  const pad = compact ? 12 : 28
  const all = data.flatMap(point => [point.live, point.mean, point.lower, point.upper])
  const rawMin = Math.min(...all)
  const rawMax = Math.max(...all)
  const margin = Math.max((rawMax - rawMin) * 0.12, 0.1)
  const min = rawMin - margin
  const max = rawMax + margin
  const span = max - min || 1
  const live = pointsFor(data.map(point => point.live), width, height, pad, min, span)
  const mean = pointsFor(data.map(point => point.mean), width, height, pad, min, span)

  return <div className={compact ? 'baseline-chart baseline-chart--compact' : 'baseline-chart'}>
    <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Live behaviour compared with historical baseline envelope">
      {!compact && Array.from({ length: 5 }).map((_, index) => {
        const y = pad + index * ((height - pad * 2) / 4)
        return <line key={index} x1={pad} x2={width - pad} y1={y} y2={y} className="chart-grid" />
      })}
      <path d={bandPath(data, width, height, pad, min, span)} className="baseline-chart__band" />
      <polyline points={mean} className="baseline-chart__mean" />
      <polyline points={live} className="baseline-chart__live" />
    </svg>
    {!compact && <div className="chart-axis"><span>{data[0]?.label}</span><span>{data[Math.floor(data.length / 2)]?.label}</span><span>{data[data.length - 1]?.label}</span></div>}
  </div>
}

export function DistributionBars({ values }: { values: number[] }) {
  const max = Math.max(...values, 1)
  return <div className="distribution-bars" aria-label="Historical distribution">
    {values.map((value, index) => <span key={index} style={{ height: `${Math.max(8, (value / max) * 100)}%` }} />)}
  </div>
}
