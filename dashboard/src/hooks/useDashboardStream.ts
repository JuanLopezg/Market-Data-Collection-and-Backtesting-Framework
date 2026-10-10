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
    let source: EventSource | null = null

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
      source?.close()
      source = null
    }
    const failed = () => {
      if (!closed) setState('RECONNECTING')
    }

    const disconnect = () => {
      source?.close()
      source = null
    }
    const connect = () => {
      if (closed || source) return
      setState('CONNECTING')
      source = new EventSource(endpoint, { withCredentials: true })
      source.addEventListener('connected', connected)
      source.addEventListener('invalidate', invalidated)
      source.addEventListener('auth-expired', authExpired)
      source.onerror = failed
    }

    // Full navigation does not unmount React. Close the old document's stream
    // before it enters browser history, and reopen it when that document returns.
    window.addEventListener('pagehide', disconnect)
    window.addEventListener('pageshow', connect)
    connect()

    return () => {
      closed = true
      window.removeEventListener('pagehide', disconnect)
      window.removeEventListener('pageshow', connect)
      disconnect()
    }
  }, [])

  return state
}
