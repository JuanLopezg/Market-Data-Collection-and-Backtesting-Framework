export class DashboardApiError extends Error {
  readonly status: number | null
  readonly endpoint: string

  constructor(message: string, endpoint: string, status: number | null = null) {
    super(message)
    this.name = 'DashboardApiError'
    this.endpoint = endpoint
    this.status = status
  }
}

export interface DashboardApiClientOptions {
  baseUrl: string
  timeoutMs: number
}

export class DashboardApiClient {
  private readonly baseUrl: string
  private readonly timeoutMs: number

  constructor(options: DashboardApiClientOptions) {
    this.baseUrl = options.baseUrl.replace(/\/$/, '')
    this.timeoutMs = options.timeoutMs
  }

  async get<T>(path: string): Promise<T> {
    return this.request<T>(path, { method: 'GET' })
  }

  async post<T>(path: string, body: unknown, headers: Record<string, string> = {}): Promise<T> {
    return this.request<T>(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...headers },
      body: JSON.stringify(body),
    })
  }

  private async request<T>(path: string, init: RequestInit): Promise<T> {
    const endpoint = `${this.baseUrl}${path}`
    const controller = new AbortController()
    const timer = window.setTimeout(() => controller.abort(), this.timeoutMs)

    try {
      const response = await fetch(endpoint, {
        ...init,
        headers: { Accept: 'application/json', ...(init.headers ?? {}) },
        credentials: 'same-origin',
        signal: controller.signal,
      })

      if (!response.ok) {
        if (response.status === 401) window.dispatchEvent(new Event('dashboard-auth-expired'))
        let detail = `Dashboard API returned HTTP ${response.status}.`
        try {
          const payload = await response.json() as { error?: string }
          if (payload.error) detail = payload.error
        } catch { /* keep generic error */ }
        throw new DashboardApiError(detail, endpoint, response.status)
      }

      const contentType = response.headers.get('content-type') ?? ''
      if (!contentType.includes('application/json')) {
        throw new DashboardApiError('Dashboard API returned a non-JSON response.', endpoint, response.status)
      }

      return await response.json() as T
    } catch (error) {
      if (error instanceof DashboardApiError) throw error
      if (error instanceof DOMException && error.name === 'AbortError') {
        throw new DashboardApiError(`Dashboard API request timed out after ${this.timeoutMs} ms.`, endpoint)
      }
      const message = error instanceof Error ? error.message : 'Unknown network error.'
      throw new DashboardApiError(`Dashboard API request failed: ${message}`, endpoint)
    } finally {
      window.clearTimeout(timer)
    }
  }
}
