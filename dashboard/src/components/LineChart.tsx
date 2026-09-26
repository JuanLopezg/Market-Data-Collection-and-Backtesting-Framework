import type { EquityPoint } from '../types/dashboard'

export function LineChart({ data, compact = false }: { data: EquityPoint[]; compact?: boolean }) {
  const width = 900, height = compact ? 240 : 300, pad = 28
  const all = data.flatMap(d => [d.equity, d.benchmark])
  const min = Math.min(...all), max = Math.max(...all), span = max - min || 1
  const make = (key: 'equity' | 'benchmark') => data.map((d, i) => `${pad+(i/(data.length-1))*(width-pad*2)},${height-pad-((d[key]-min)/span)*(height-pad*2)}`).join(' ')
  return (
    <div className="chart-wrap">
      <svg viewBox={`0 0 ${width} ${height}`} className="line-chart" role="img" aria-label="Time series chart">
        {Array.from({ length: 5 }).map((_, i) => <line key={i} x1={pad} x2={width-pad} y1={pad+i*((height-pad*2)/4)} y2={pad+i*((height-pad*2)/4)} className="chart-grid" />)}
        <polyline points={make('benchmark')} className="chart-line chart-line--muted" />
        <polyline points={make('equity')} className="chart-line chart-line--primary" />
      </svg>
      <div className="chart-axis"><span>{data[0]?.label}</span><span>{data[Math.floor(data.length/2)]?.label}</span><span>{data[data.length - 1]?.label}</span></div>
    </div>
  )
}
