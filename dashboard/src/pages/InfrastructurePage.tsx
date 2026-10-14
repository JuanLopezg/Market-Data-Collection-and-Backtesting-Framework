import { useState } from 'react'
import { DataLoadError } from '../components/DataLoadError'
import { DEFAULT_HOST_RANGE, HOST_PRESETS, HostResourceCharts } from '../components/HostResourceCharts'
import { TimeRangePicker } from '../components/TimeRangePicker'
import type { TimeRange } from '../components/TimeRangePicker'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { InfrastructureData, InfrastructureHealth } from '../types/dashboard'

// Infrastructure: one square per component (green up, orange degraded, red down, grey
// unknown) and host CPU/RAM/disk over time. Nothing else.

type ComponentState = 'UP' | 'WARN' | 'DOWN' | 'UNKNOWN'

interface ComponentStatus {
  name: string
  state: ComponentState
}

function fromHealth(health: InfrastructureHealth): ComponentState {
  return health === 'HEALTHY' ? 'UP' : health === 'WARN' ? 'WARN' : health === 'CRITICAL' ? 'DOWN' : 'UNKNOWN'
}

// Containers, trading processes and the shared services, whatever the provider observed.
function components(data: InfrastructureData): ComponentStatus[] {
  const rows: ComponentStatus[] = []
  for (const container of data.containers) {
    const state: ComponentState = container.state !== 'RUNNING' ? 'DOWN'
      : container.health === 'CRITICAL' ? 'DOWN' : container.health === 'WARN' ? 'WARN' : 'UP'
    rows.push({ name: container.name, state })
  }
  for (const service of data.services) {
    const state: ComponentState = service.processState === 'RUNNING' ? 'UP'
      : service.processState === 'STOPPED' || service.processState === 'MISSING' ? 'DOWN'
      : service.processState === 'UNKNOWN' ? 'UNKNOWN' : fromHealth(service.health)
    rows.push({ name: service.service, state })
  }
  // Shared services only when the provider actually uses them.
  if (data.postgres.state !== 'UNKNOWN') rows.push({ name: 'PostgreSQL', state: data.postgres.connected ? fromHealth(data.postgres.state) : 'DOWN' })
  if (data.nats.state !== 'UNKNOWN') rows.push({ name: 'NATS', state: data.nats.connected ? fromHealth(data.nats.state) : 'DOWN' })
  if (data.exchange.state !== 'UNKNOWN') rows.push({ name: `Exchange · ${data.exchange.venue}`, state: data.exchange.connected ? fromHealth(data.exchange.state) : 'DOWN' })
  return rows
}

const STATE_LABEL: Record<ComponentState, string> = { UP: 'Up', WARN: 'Degraded', DOWN: 'Down', UNKNOWN: 'Unknown' }

export function InfrastructurePage() {
  const { data, error, retry } = useDashboardResource('getInfrastructure')
  // Time range of the CPU/RAM/disk charts, picked in the page heading like Overview.
  const [range, setRange] = useState<TimeRange>(DEFAULT_HOST_RANGE)

  if (error) return <DataLoadError title="Infrastructure data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading infrastructure…</div>

  const rows = components(data)
  return <>
    <div className="page-heading">
      <h1>Infrastructure</h1>
      <TimeRangePicker presets={HOST_PRESETS} value={range} onChange={setRange} precision="datetime"/>
    </div>

    <section className="component-grid">
      {rows.length === 0 && <div className="empty-cell">No components observed yet</div>}
      {rows.map(row => <div className={`component-tile component-tile--${row.state.toLowerCase()}`} key={row.name}>
        <strong>{row.name}</strong>
        <span className="component-tile__state">{STATE_LABEL[row.state]}</span>
      </div>)}
    </section>

    <HostResourceCharts range={range} onRangeChange={setRange}/>
  </>
}
