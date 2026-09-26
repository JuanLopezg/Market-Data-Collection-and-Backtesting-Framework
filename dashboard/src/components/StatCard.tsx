import type { StatMetric } from '../types/dashboard'
import { Sparkline } from './Sparkline'

export function StatCard({ metric }: { metric: StatMetric }) {
  return (
    <div className="stat-card">
      <div>
        <div className="stat-card__label">{metric.label}</div>
        <div className="stat-card__value">{metric.value}</div>
        <div className="stat-card__delta">{metric.delta} <span>{metric.detail}</span></div>
      </div>
      {metric.trend && <Sparkline values={metric.trend} />}
    </div>
  )
}
