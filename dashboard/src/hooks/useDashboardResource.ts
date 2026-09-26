import { useCallback, useEffect, useState } from 'react'
import type { DashboardDataSource } from '../data/dashboardDataSource'
import { useDashboardDataSource } from '../providers/DashboardDataSourceProvider'
import { DASHBOARD_STREAM_INVALIDATE_EVENT, type DashboardStreamInvalidation } from './useDashboardStream'

type ResourceKey = keyof DashboardDataSource
type ResourceResult<K extends ResourceKey> = Awaited<ReturnType<DashboardDataSource[K]>>

const streamResourceName: Partial<Record<ResourceKey, string>> = {
  getProviderStatus: 'provider-status',
  getDiagnostics: 'diagnostics',
  getSafetyGate: 'safety-gate',
  getGlobalReadiness: 'global-readiness',
  getSymbolRegistry: 'symbol-registry',
  getLedger: 'execution',
  getShellStatus: 'shell-status',
  getOverview: 'overview',
  getPositions: 'positions',
  getReconciliation: 'reconciliation',
  getPipeline: 'pipeline',
  getExecution: 'execution',
  getRisk: 'risk',
  getMarketData: 'market-data',
  getInfrastructure: 'infrastructure',
  getAlertsAudit: 'alerts-audit',
  getLiveVsExpected: 'live-vs-expected',
  getManualControl: 'manual-control',
}

export interface DashboardResourceState<T> {
  data: T | null
  error: Error | null
  loading: boolean
  retry: () => void
}

export function useDashboardResource<K extends ResourceKey>(resource: K): DashboardResourceState<ResourceResult<K>> {
  const dataSource = useDashboardDataSource()
  const [data, setData] = useState<ResourceResult<K> | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const [loading, setLoading] = useState(true)
  const [attempt, setAttempt] = useState(0)

  const retry = useCallback(() => setAttempt(value => value + 1), [])

  useEffect(() => {
    const resourceName = streamResourceName[resource]
    if (!resourceName) return
    let pendingRefresh: number | null = null
    const onInvalidate = (event: Event) => {
      const detail = (event as CustomEvent<DashboardStreamInvalidation>).detail
      if (!detail?.resources) return
      if (detail.resources.includes('*') || detail.resources.includes(resourceName)) {
        if (pendingRefresh !== null) return
        pendingRefresh = window.setTimeout(() => {
          pendingRefresh = null
          setAttempt(value => value + 1)
        }, 200)
      }
    }
    window.addEventListener(DASHBOARD_STREAM_INVALIDATE_EVENT, onInvalidate)
    return () => {
      window.removeEventListener(DASHBOARD_STREAM_INVALIDATE_EVENT, onInvalidate)
      if (pendingRefresh !== null) window.clearTimeout(pendingRefresh)
    }
  }, [resource])

  useEffect(() => {
    let active = true
    setLoading(true)
    setError(null)

    // Keep the DashboardDataSource method bound to its instance.
    // ApiDashboardDataSource methods use `this.client`; extracting a method and
    // calling it unbound throws synchronously and used to blank the app after login.
    const loader = dataSource[resource].bind(dataSource) as () => Promise<ResourceResult<K>>
    Promise.resolve()
      .then(loader)
      .then(result => {
        if (!active) return
        setData(result)
      })
      .catch(cause => {
        if (!active) return
        setError(cause instanceof Error ? cause : new Error('Unknown dashboard data error.'))
      })
      .finally(() => {
        if (active) setLoading(false)
      })

    return () => { active = false }
  }, [attempt, dataSource, resource])

  return { data, error, loading, retry }
}
