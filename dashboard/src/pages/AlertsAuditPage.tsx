import { AlertOctagon, AlertTriangle, CheckCircle2, CircleCheckBig, Filter, History, Search, ShieldCheck } from 'lucide-react'
import { useMemo, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { AlertsAuditData, AlertStatus, Severity } from '../types/dashboard'

function SeverityPill({ severity }: { severity: Severity }) {
  const tone = severity === 'CRITICAL' ? 'bad' : severity === 'WARN' ? 'warn' : 'info'
  return <span className={`alert-severity alert-severity--${tone}`}>{severity}</span>
}

function AlertStatusPill({ status }: { status: AlertStatus }) {
  const tone = status === 'ACTIVE' ? 'bad' : status === 'ACKNOWLEDGED' ? 'warn' : 'good'
  return <span className={`alert-status alert-status--${tone}`}>{status}</span>
}

export function AlertsAuditPage() {
  const { data, error, retry } = useDashboardResource('getAlertsAudit')
  const [severity, setSeverity] = useState<'ALL' | Severity>('ALL')
  const [status, setStatus] = useState<'ALL' | AlertStatus>('ALL')
  const [query, setQuery] = useState('')

  const filteredAlerts = useMemo(() => {
    if (!data) return []
    const q = query.trim().toLowerCase()
    return data.alerts.filter(alert => (severity === 'ALL' || alert.severity === severity) && (status === 'ALL' || alert.status === status) && (!q || `${alert.title} ${alert.detail} ${alert.service} ${alert.asset ?? ''} ${alert.correlationId ?? ''}`.toLowerCase().includes(q)))
  }, [data, severity, status, query])

  if (error) return <DataLoadError title="Alerts and audit unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading alerts and audit…</div>

  const real = data.sourceMode === 'REAL'
  const ackAvailable = data.acknowledgementAvailable !== false
  const historyAvailable = data.durableAlertHistoryAvailable !== false
  const humanAuditAvailable = data.humanAuditAvailable !== false
  const durableEvents = data.durableLifecycleEvents ?? []

  return <>
    <div className="page-heading">
      <div><h1>Alerts & Audit</h1><p>Operational alerts, system evidence and durable audit boundaries.</p></div>
      <div className="cycle-label"><History size={14}/><span>{real ? 'Real operational evidence' : 'Operational timeline'}</span></div>
    </div>

    {real && <div className="real-source-banner"><strong>REAL DATA · Step 43 alert lifecycle + Step 46 manual intent audit</strong><span>{data.sourceNote} {data.sourceUpdatedAt ? `Checked ${data.sourceUpdatedAt}.` : ''}</span></div>}

    {real && <Panel title="Step 43 · Durable Alert Watchdog" right={<span className="muted">{data.watchdogState ?? 'UNKNOWN'}</span>}>
      <div className="alerts-summary">
        <div className="mini-metric"><span>Watchdog state</span><strong className={data.watchdogState === 'HEALTHY' ? 'positive' : 'warning'}>{data.watchdogState ?? 'UNKNOWN'}</strong><small>Independent of the browser</small></div>
        <div className="mini-metric"><span>Durable events</span><strong>{data.durableEventCount ?? 0}</strong><small>OPENED / UPDATED / RESOLVED transitions</small></div>
        <div className="mini-metric"><span>Last sweep</span><strong>{data.watchdogLastSweepAt ? data.watchdogLastSweepAt.slice(11,19) : '—'}</strong><small>{data.watchdogLastSweepAt ?? 'No persisted heartbeat yet'}</small></div>
        <div className="mini-metric"><span>Human acknowledgement</span><strong>{ackAvailable ? data.acknowledged : 'Not wired'}</strong><small>{ackAvailable ? 'Persisted acknowledgement state' : 'Deliberately deferred; no mutation endpoint'}</small></div>
      </div>
      {!data.watchdogAvailable && <div className="alerts-gap-note"><strong>Watchdog store not ready.</strong><span>The current alert projection remains read-only/fail-closed. The separate watchdog must persist a successful heartbeat before durable history is claimed.</span></div>}
    </Panel>}

    <section className="alerts-summary">
      <div className="mini-metric mini-metric--critical"><span>Active critical</span><strong className="negative">{data.activeCritical}</strong><small>Current derived conditions</small></div>
      <div className="mini-metric"><span>Active warnings</span><strong className="warning">{data.activeWarnings}</strong><small>Current derived conditions</small></div>
      <div className="mini-metric"><span>Durable lifecycle</span><strong className={historyAvailable ? 'positive' : ''}>{historyAvailable ? (data.durableEventCount ?? durableEvents.length) : 'Not wired'}</strong><small>{historyAvailable ? 'Persisted alert transitions' : 'No durable alert-history window yet'}</small></div>
      <div className="mini-metric"><span>Resolved history</span><strong className={historyAvailable ? 'positive' : ''}>{historyAvailable ? data.resolved24h : 'Not wired'}</strong><small>{historyAvailable ? 'Resolved in the last 24h' : 'No durable alert-history window yet'}</small></div>
    </section>

    <Panel title="Alert Center" right={<span className="muted">{real ? 'Current conditions derived from canonical sources' : 'Critical stays visible until acknowledged / resolved'}</span>}>
      <div className="alert-toolbar">
        <div className="search-box"><Search size={13}/><input value={query} onChange={e => setQuery(e.target.value)} placeholder="Search service, asset, event or correlation ID"/></div>
        <label><Filter size={12}/><span>Severity</span><select value={severity} onChange={e => setSeverity(e.target.value as 'ALL' | Severity)}><option>ALL</option><option>CRITICAL</option><option>WARN</option><option>INFO</option></select></label>
        <label><span>Status</span><select value={status} onChange={e => setStatus(e.target.value as 'ALL' | AlertStatus)}><option>ALL</option><option>ACTIVE</option><option>ACKNOWLEDGED</option><option>RESOLVED</option></select></label>
      </div>
      <div className="alert-list">{filteredAlerts.map(alert => <div className={`alert-row alert-row--${alert.severity.toLowerCase()}`} key={alert.id}>
        <div className="alert-row__icon">{alert.severity === 'CRITICAL' ? <AlertOctagon size={16}/> : alert.severity === 'WARN' ? <AlertTriangle size={16}/> : <CheckCircle2 size={16}/>}</div>
        <div className="alert-row__main"><div><strong>{alert.title}</strong><SeverityPill severity={alert.severity}/><AlertStatusPill status={alert.status}/></div><p>{alert.detail}</p><small>{alert.timestamp} · {alert.service}{alert.asset ? ` · ${alert.asset}` : ''} · {alert.eventType}</small></div>
        <div className="alert-row__meta">{alert.correlationId && <code>{alert.correlationId}</code>}{alert.linkedContext && <span>{alert.linkedContext}</span>}</div>
      </div>)}{filteredAlerts.length === 0 && <div className="empty-state">No alerts match the current filters.</div>}</div>
    </Panel>

    {real && historyAvailable && <Panel title="Durable Alert Lifecycle" right={<span className="muted"><History size={12}/> append-only observability evidence</span>}>
      <div className="table-wrap"><table className="wide-table audit-table"><thead><tr><th>Recorded</th><th>Transition</th><th>Severity</th><th>Service</th><th>Event</th><th>Asset</th><th>Title</th></tr></thead><tbody>{durableEvents.map(event => <tr key={event.eventId}><td>{event.recordedAt}</td><td><strong>{event.transition}</strong></td><td><SeverityPill severity={event.alert.severity}/></td><td>{event.alert.service}</td><td>{event.alert.eventType}</td><td>{event.alert.asset ?? '—'}</td><td>{event.alert.title}</td></tr>)}{durableEvents.length === 0 && <tr><td colSpan={7}>No lifecycle transition has been persisted yet. This is valid when the current system has not opened/resolved an alert since the watchdog store was initialized.</td></tr>}</tbody></table></div>
    </Panel>}

    <Panel title={real ? 'System Evidence Timeline' : 'Audit Trail'} right={<span className="muted"><ShieldCheck size={12}/> {real ? (humanAuditAvailable ? 'canonical evidence · durable manual intent' : 'canonical evidence · human audit unavailable') : 'append-only UI model'}</span>}>
      {real && !humanAuditAvailable && <div className="alerts-gap-note"><strong>Durable manual-intent audit is unavailable.</strong><span>System evidence remains reconstructed from canonical runtime sources, but Step 46 manual route admission records cannot be claimed durable until the separate operator-intent store is readable. Alert acknowledgement remains deliberately deferred.</span></div>}
      <div className="table-wrap"><table className="wide-table audit-table"><thead><tr><th>Timestamp</th><th>Actor</th><th>Type</th><th>Action</th><th>Target</th><th>Result</th><th>Correlation</th><th>Detail</th></tr></thead><tbody>{data.audit.map(row => <tr key={row.id}><td>{row.timestamp}</td><td><strong>{row.actor}</strong></td><td>{row.actorType}</td><td>{row.action}</td><td>{row.target}</td><td><span className={`audit-result audit-result--${row.result.toLowerCase()}`}>{row.result === 'SUCCESS' ? <CircleCheckBig size={11}/> : row.result === 'REJECTED' ? <AlertTriangle size={11}/> : <CheckCircle2 size={11}/>} {row.result}</span></td><td>{row.correlationId ? <code>{row.correlationId}</code> : '—'}</td><td>{row.detail}</td></tr>)}</tbody></table></div>
    </Panel>
  </>
}
