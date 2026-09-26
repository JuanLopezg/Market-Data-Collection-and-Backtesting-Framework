import type { AuthLoginResponse, AuthSession } from '../types/auth'
import { runtimeConfig } from '../config/runtime'

export class AuthApiError extends Error {
  readonly status: number | null

  constructor(message: string, status: number | null = null) {
    super(message)
    this.name = 'AuthApiError'
    this.status = status
  }
}

async function parseError(response: Response) {
  try {
    const body = await response.json() as { error?: string }
    return body.error ?? `HTTP ${response.status}`
  } catch {
    return `HTTP ${response.status}`
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const controller = new AbortController()
  const timer = window.setTimeout(() => controller.abort(), runtimeConfig.apiTimeoutMs)

  try {
    const response = await fetch(`${runtimeConfig.apiBaseUrl}${path}`, {
      ...init,
      credentials: 'same-origin',
      headers: {
        Accept: 'application/json',
        ...(init.body ? { 'Content-Type': 'application/json' } : {}),
        ...(init.headers ?? {}),
      },
      signal: controller.signal,
    })

    if (!response.ok) {
      throw new AuthApiError(await parseError(response), response.status)
    }

    return await response.json() as T
  } catch (error) {
    if (error instanceof AuthApiError) throw error
    if (error instanceof DOMException && error.name === 'AbortError') {
      throw new AuthApiError('Authentication request timed out.')
    }
    throw new AuthApiError(error instanceof Error ? error.message : 'Authentication request failed.')
  } finally {
    window.clearTimeout(timer)
  }
}

export const authClient = {
  me: () => request<AuthSession>('/auth/me'),
  login: (username: string, password: string) => request<AuthLoginResponse>('/auth/login', {
    method: 'POST',
    body: JSON.stringify({ username, password }),
  }),
  logout: (csrfToken: string) => request<{ status: 'ok' }>('/auth/logout', {
    method: 'POST',
    headers: { 'X-CSRF-Token': csrfToken },
  }),
}
