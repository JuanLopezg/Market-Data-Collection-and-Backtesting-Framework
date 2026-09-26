import { runtimeConfig } from '../config/runtime'
import { DashboardApiClient } from './apiClient'
import { ApiDashboardDataSource } from './apiDashboardDataSource'
import type { DashboardDataSource } from './dashboardDataSource'
import { mockDashboardDataSource } from './mockDashboardDataSource'

export function createDashboardDataSource(): DashboardDataSource {
  if (runtimeConfig.dataMode === 'api') {
    return new ApiDashboardDataSource(new DashboardApiClient({
      baseUrl: runtimeConfig.apiBaseUrl,
      timeoutMs: runtimeConfig.apiTimeoutMs,
    }))
  }

  return mockDashboardDataSource
}
