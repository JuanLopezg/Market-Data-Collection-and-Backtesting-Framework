import { Activity, LockKeyhole, ShieldCheck } from 'lucide-react'
import { FormEvent, useState } from 'react'
import { Navigate, useLocation, useNavigate } from 'react-router-dom'
import { AuthApiError } from '../data/authClient'
import { useAuth } from '../providers/AuthProvider'

interface LoginLocationState {
  from?: string
  authError?: string | null
}

export function LoginPage() {
  const { session, login } = useAuth()
  const navigate = useNavigate()
  const location = useLocation()
  const state = (location.state ?? {}) as LoginLocationState
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState(state.authError ?? '')

  if (session) return <Navigate to={state.from || '/'} replace />

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    setSubmitting(true)
    setError('')
    try {
      await login(username.trim(), password)
      navigate(state.from || '/', { replace: true })
    } catch (err) {
      setError(err instanceof AuthApiError ? err.message : 'Login failed.')
    } finally {
      setSubmitting(false)
    }
  }

  return <div className="login-page">
    <div className="login-card">
      <div className="login-brand"><div className="login-brand__icon"><Activity size={22}/></div><div><strong>Control Dashboard</strong><span>Algo Trading Operations</span></div></div>
      <div className="login-copy"><h1>Secure access</h1><p>Authenticate before accessing portfolio, execution, risk, infrastructure and audit data.</p></div>
      <form onSubmit={submit} className="login-form">
        <label><span>Username</span><input autoFocus autoComplete="username" value={username} onChange={e => setUsername(e.target.value)} placeholder="viewer or operator" required/></label>
        <label><span>Password</span><input type="password" autoComplete="current-password" value={password} onChange={e => setPassword(e.target.value)} placeholder="••••••••••••" required/></label>
        {error && <div className="login-error"><LockKeyhole size={14}/><span>{error}</span></div>}
        <button className="button-primary login-submit" type="submit" disabled={submitting}>{submitting ? 'Signing in…' : 'Sign in'}</button>
      </form>
      <div className="login-security"><ShieldCheck size={15}/><span>HttpOnly session cookie · SameSite=Strict · role-based access · CSRF token for state-changing requests</span></div>
    </div>
  </div>
}
