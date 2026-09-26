import { createContext, useContext, useMemo, type PropsWithChildren } from 'react'
import type { DashboardDataSource } from '../data/dashboardDataSource'
import { createDashboardDataSource } from '../data/dashboardDataSourceFactory'

const DashboardDataSourceContext = createContext<DashboardDataSource | null>(null)

export function DashboardDataSourceProvider({ children }: PropsWithChildren) {
  const dataSource = useMemo(() => createDashboardDataSource(), [])
  return <DashboardDataSourceContext.Provider value={dataSource}>{children}</DashboardDataSourceContext.Provider>
}

export function useDashboardDataSource() {
  const value = useContext(DashboardDataSourceContext)
  if (!value) throw new Error('useDashboardDataSource must be used within DashboardDataSourceProvider.')
  return value
}
