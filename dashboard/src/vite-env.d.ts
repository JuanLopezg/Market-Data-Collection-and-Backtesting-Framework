/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_DASHBOARD_DATA_MODE?: 'mock' | 'api'
  readonly VITE_DASHBOARD_API_BASE_URL?: string
  readonly VITE_DASHBOARD_API_TIMEOUT_MS?: string
  readonly VITE_DASHBOARD_MOCK_LATENCY_MS?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
