import { useEffect, useState } from 'react'
import { runtimeConfig } from '../config/runtime'

export type DashboardStreamState = 'OFF' | 'CONNECTING' | 'LIVE' | 'RECONNECTING'

export interface DashboardStreamInvalidation {
  resources: string[]
  subjects?: string[]
  reason: string
  at: string
}

export const DASHBOARD_STREAM_INVALIDATE_EVENT = 'dashboard-stream-invalidate'

export function useDashboardStream() {
  const [state, setState] = useState<DashboardStreamState>(runtimeConfig.dataMode === 'api' ? 'CONNECTING' : 'OFF')

  useEffect(() => {
    if (runtimeConfig.dataMode !== 'api') {
      setState('OFF')
      return
    }

    let closed = false
    const endpoint = `${runtimeConfig.apiBaseUrl}/stream`
    const source = new EventSource(endpoint, { withCredentials: true })

    const connected = () => {
      if (!closed) setState('LIVE')
    }
    const invalidated = (event: Event) => {
      if (!(event instanceof MessageEvent)) return
      try {
        const detail = JSON.parse(event.data) as DashboardStreamInvalidation
        if (!Array.isArray(detail.resources)) return
        window.dispatchEvent(new CustomEvent<DashboardStreamInvalidation>(DASHBOARD_STREAM_INVALIDATE_EVENT, { detail }))
      } catch {
        // Ignore malformed stream events; REST remains the authoritative data path.
      }
    }
    const authExpired = () => {
      window.dispatchEvent(new Event('dashboard-auth-expired'))
      source.close()
    }
    const failed = () => {
      if (!closed) setState('RECONNECTING')
    }

    source.addEventListener('connected', connected)
    source.addEventListener('invalidate', invalidated)
    source.addEventListener('auth-expired', authExpired)
    source.onerror = failed

    return () => {
      closed = true
      source.close()
    }
  }, [])

  return state
}
