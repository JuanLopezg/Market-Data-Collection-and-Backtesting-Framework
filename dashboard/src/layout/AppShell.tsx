import { Activity, ArrowLeftRight, Bell, Boxes, ChartLine, CircleDollarSign, Gauge, Home, LogOut, Network, Shield, SlidersHorizontal, UserRound, Waypoints } from 'lucide-react'
import { Link, NavLink, Outlet } from 'react-router-dom'
import { runtimeConfig } from '../config/runtime'
import { useDashboardResource } from '../hooks/useDashboardResource'
import { useDashboardStream } from '../hooks/useDashboardStream'
import { useAuth } from '../providers/AuthProvider'

const nav = [
  ['Overview', '/', Home],
  ['Positions', '/positions', CircleDollarSign],
  ['Pipeline', '/pipeline', Waypoints],
  ['Execution', '/execution', Activity],
  ['Risk', '/risk', Shield],
  ['Market Data', '/market-data', ChartLine],
  ['Symbol Mapping', '/symbol-mapping', ArrowLeftRight],
  ['Infrastructure', '/infrastructure', Network],
  ['Manual Control', '/manual-control', SlidersHorizontal],
  ['Alerts & Audit', '/alerts-audit', Bell],
  ['Live vs Expected', '/live-vs-expected', Gauge],
] as const

export function AppShell() {
  const { data: status, error: statusError, retry: retryStatus } = useDashboardResource('getShellStatus')
  const { data: providerStatus } = useDashboardResource('getProviderStatus')
  const { session, logout } = useAuth()
  const streamState = useDashboardStream()
  const role = session?.user.role ?? 'VIEWER'

  const sidebarStatus = statusError ? 'Data source offline' : status?.readiness === 'READY' ? 'System Ready' : status?.readiness ? `System ${status.readiness}` : 'System Status'
  const sidebarDetail = status?.readinessDetail ?? (statusError ? 'Click to retry shell status' : 'Global readiness loading')

  return <div className="app-shell">
    <aside className="sidebar">
      <div className="brand"><Boxes size={22}/><div><strong>Control Dashboard</strong><small>Algo Trading Operations</small></div></div>
      <nav>{nav.map(([label, path, Icon]) => <NavLink key={label} to={path} end={path === '/'}><Icon size={18}/><span>{label}</span></NavLink>)}</nav>
      <div className="sidebar__identity"><UserRound size={15}/><div><strong>{session?.user.username}</strong><small>{role}</small></div><button type="button" title="Sign out" aria-label="Sign out" onClick={() => void logout()}><LogOut size={15}/></button></div>
      <div className="sidebar__footer" title={sidebarDetail}>
        <span className={`dot ${status?.readiness === 'READY' ? 'dot--good' : status?.readiness === 'PAUSED' || statusError ? 'dot--bad' : 'dot--warn'}`}/> {sidebarStatus}
        <small>{sidebarDetail}</small>
        <button className={`source-mode ${providerStatus?.mode === 'mock' ? 'source-mode--mock' : ''}`} type="button" onClick={statusError ? retryStatus : undefined}>{providerStatus ? `${providerStatus.mode.toUpperCase()} DATA · ${providerStatus.name}` : `${runtimeConfig.dataMode.toUpperCase()} · ${runtimeConfig.dataMode === 'api' ? runtimeConfig.apiBaseUrl : 'local mocks'}`}</button>
      </div>
    </aside>

    <div className="workspace">
      <header className="topbar">
        <div className="topbar__spacer"/>
        <span className={`stream-state stream-state--${streamState.toLowerCase()}`} title="Authenticated Server-Sent Events stream; NATS only invalidates REST read models and never reaches the browser directly"><span className="stream-state__dot"/>{streamState === 'LIVE' ? 'SSE LIVE' : streamState}</span>
        <Link className="topbar__alert" to="/alerts-audit" title={status ? `${status.criticalAlertCount ?? 0} critical · ${status.warningAlertCount ?? status.alertCount} warning` : 'Alerts & Audit'}><Bell size={18}/>{Boolean(status?.alertCount) && <span>{status?.alertCount}</span>}</Link>
      </header>
      <main><Outlet /></main>
    </div>
  </div>
}
