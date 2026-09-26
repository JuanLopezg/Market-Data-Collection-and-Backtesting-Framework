export type DashboardDataMode = 'mock' | 'api'

function readMode(value: string | undefined): DashboardDataMode {
  return value?.toLowerCase() === 'api' ? 'api' : 'mock'
}

function readPositiveInteger(value: string | undefined, fallback: number) {
  const parsed = Number(value)
  return Number.isFinite(parsed) && parsed > 0 ? Math.round(parsed) : fallback
}

const apiBaseUrl = (import.meta.env.VITE_DASHBOARD_API_BASE_URL ?? '/api').replace(/\/$/, '')

export const runtimeConfig = Object.freeze({
  dataMode: readMode(import.meta.env.VITE_DASHBOARD_DATA_MODE),
  apiBaseUrl,
  apiTimeoutMs: readPositiveInteger(import.meta.env.VITE_DASHBOARD_API_TIMEOUT_MS, 8_000),
  mockLatencyMs: readPositiveInteger(import.meta.env.VITE_DASHBOARD_MOCK_LATENCY_MS, 80),
})
