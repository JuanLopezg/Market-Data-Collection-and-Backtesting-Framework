import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { authClient, AuthApiError } from '../data/authClient'
import type { AuthSession, UserRole } from '../types/auth'
import { runtimeConfig } from '../config/runtime'

interface AuthContextValue {
  session: AuthSession | null
  loading: boolean
  error: string | null
  login: (username: string, password: string) => Promise<void>
  logout: () => Promise<void>
  hasRole: (role: UserRole) => boolean
  refresh: () => Promise<void>
}

const AuthContext = createContext<AuthContextValue | null>(null)

export function AuthProvider({ children }: { children: ReactNode }) {
  const [session, setSession] = useState<AuthSession | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  const refresh = async () => {
    setLoading(true)
    if (runtimeConfig.dataMode === 'mock') {
      setSession({ authenticated: true, user: { username: 'mock-operator', role: 'OPERATOR' }, csrfToken: 'mock-csrf', expiresAt: 'mock-session' })
      setError(null)
      setLoading(false)
      return
    }
    try {
      const next = await authClient.me()
      setSession(next)
      setError(null)
    } catch (err) {
      if (err instanceof AuthApiError && err.status === 401) {
        setSession(null)
        setError(null)
      } else {
        setSession(null)
        setError(err instanceof Error ? err.message : 'Could not check session.')
      }
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => { void refresh() }, [])

  useEffect(() => {
    const onExpired = () => { void refresh() }
    window.addEventListener('dashboard-auth-expired', onExpired)
    return () => window.removeEventListener('dashboard-auth-expired', onExpired)
  }, [])

  const login = async (username: string, password: string) => {
    if (runtimeConfig.dataMode === 'mock') {
      if (password !== 'demo' || !['viewer', 'operator'].includes(username.toLowerCase())) {
        throw new AuthApiError('Mock login: use viewer/demo or operator/demo.', 401)
      }
      const role: UserRole = username.toLowerCase() === 'operator' ? 'OPERATOR' : 'VIEWER'
      setSession({ authenticated: true, user: { username: `mock-${username.toLowerCase()}`, role }, csrfToken: 'mock-csrf', expiresAt: 'mock-session' })
      setError(null)
      return
    }
    const next = await authClient.login(username, password)
    setSession(next)
    setError(null)
  }

  const logout = async () => {
    if (runtimeConfig.dataMode === 'api' && session?.csrfToken) {
      await authClient.logout(session.csrfToken)
    }
    setSession(null)
  }

  const value = useMemo<AuthContextValue>(() => ({
    session,
    loading,
    error,
    login,
    logout,
    hasRole: role => session?.user.role === role,
    refresh,
  }), [session, loading, error])

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>
}

export function useAuth() {
  const value = useContext(AuthContext)
  if (!value) throw new Error('useAuth must be used inside AuthProvider')
  return value
}
