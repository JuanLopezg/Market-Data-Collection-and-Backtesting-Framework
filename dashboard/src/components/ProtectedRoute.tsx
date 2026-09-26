import { Navigate, Outlet, useLocation } from 'react-router-dom'
import { useAuth } from '../providers/AuthProvider'

export function ProtectedRoute() {
  const { session, loading, error } = useAuth()
  const location = useLocation()

  if (loading) {
    return <div className="auth-loading"><div className="auth-loading__mark">CD</div><span>Checking secure session…</span></div>
  }

  if (!session) {
    return <Navigate to="/login" replace state={{ from: location.pathname, authError: error }} />
  }

  return <Outlet />
}
