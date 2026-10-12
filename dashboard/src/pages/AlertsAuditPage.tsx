import { Search } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useAuth } from '../providers/AuthProvider'
import { useDashboardDataSource } from '../providers/DashboardDataSourceProvider'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { AlertStatus, Severity } from '../types/dashboard'

// Alerts & Audit: what needs attention now, and who did what. Alerts are notices only;
// acknowledging one records that an operator saw it, it does not resolve the condition.

const SEVERITY_ORDER: Record<Severity, number> = { CRITICAL: 0, WARN: 1, INFO: 2 }
const SEVERITY_LABEL: Record<Severity, string> = { CRITICAL: 'Critical', WARN: 'Warning', INFO: 'Info' }
const STATUS_LABEL: Record<AlertStatus, string> = { ACTIVE: 'Open', ACKNOWLEDGED: 'Seen', RESOLVED: 'Resolved' }
const PAGE = 20

function SeverityPill({ severity }: { severity: Severity }) {
  const tone = severity === 'CRITICAL' ? 'bad' : severity === 'WARN' ? 'warn' : 'info'
  return <span className={`alert-severity alert-severity--${tone}`}>{SEVERITY_LABEL[severity]}</span>
}

export function AlertsAuditPage() {
  const { data, error, retry } = useDashboardResource('getAlertsAudit')
  const [severity, setSeverity] = useState<'ALL' | Severity>('ALL')
  const [query, setQuery] = useState('')
  const [auditLimit, setAuditLimit] = useState(PAGE)
  const dataSource = useDashboardDataSource()
  const { session } = useAuth()
  const isOperator = session?.user.role === 'OPERATOR'
  const [ackBusyId, setAckBusyId] = useState<string | null>(null)
  const [ackError, setAckError] = useState<string | null>(null)

  const alerts = useMemo(() => {
    if (!data) return []
    const q = query.trim().toLowerCase()
    return data.alerts
      .filter(alert => alert.status !== 'RESOLVED')
      .filter(alert => severity === 'ALL' || alert.severity === severity)
      .filter(alert => !q || `${alert.title} ${alert.detail} ${alert.service} ${alert.asset ?? ''}`.toLowerCase().includes(q))
      .sort((a, b) => SEVERITY_ORDER[a.severity] - SEVERITY_ORDER[b.severity])
  }, [data, severity, query])

  const acknowledge = async (alertId: string, acknowledgementKey?: string) => {
    if (!isOperator || !session?.csrfToken || !acknowledgementKey) return
    setAckBusyId(alertId)
    setAckError(null)
    try {
      await dataSource.acknowledgeAlert({ alertId, acknowledgementKey, comment: '' }, session.csrfToken)
      window.setTimeout(() => retry(), 1500)
    } catch (cause) {
      setAckError(cause instanceof Error ? cause.message : 'Acknowledgement failed.')
    } finally {
      setAckBusyId(null)
    }
  }

  if (error) return <DataLoadError title="Alerts and audit unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading alerts and audit…</div>

  const ackAvailable = data.acknowledgementAvailable !== false
  const history = data.durableLifecycleEvents ?? []
  const open = data.alerts.filter(alert => alert.status !== 'RESOLVED')
  const info = open.filter(alert => alert.severity === 'INFO').length

  return <>
    <div className="page-heading"><h1>Alerts & Audit</h1></div>

    <section className="overview-stats">
      <div className="overview-stat"><span>Critical</span><strong className={data.activeCritical > 0 ? 'negative' : ''}>{data.activeCritical}</strong></div>
      <div className="overview-stat"><span>Warnings</span><strong className={data.activeWarnings > 0 ? 'leverage--elevated' : ''}>{data.activeWarnings}</strong></div>
      <div className="overview-stat"><span>Info</span><strong>{info}</strong></div>
      {ackAvailable && <div className="overview-stat"><span>Seen by Operator</span><strong>{data.acknowledged}</strong></div>}
    </section>

    {ackError && <div className="manual-banner manual-banner--bad">Acknowledgement failed: {ackError}</div>}

    <Panel title="Open Alerts">
      <div className="alert-toolbar">
        <div className="search-box"><Search size={13}/><input value={query} onChange={e => setQuery(e.target.value)} placeholder="Search alerts"/></div>
        <label><span>Severity</span><select value={severity} onChange={e => setSeverity(e.target.value as 'ALL' | Severity)}>
          <option value="ALL">All</option><option value="CRITICAL">Critical</option><option value="WARN">Warning</option><option value="INFO">Info</option>
        </select></label>
      </div>
      <div className="table-wrap"><table className="wide-table alerts-table">
        <thead><tr><th>Severity</th><th>What happened</th><th>Area</th><th>Since</th><th>Status</th>{ackAvailable && <th/>}</tr></thead>
        <tbody>
          {alerts.map(alert => <tr key={alert.id}>
            <td><SeverityPill severity={alert.severity}/></td>
            <td><strong>{alert.title}</strong>{alert.asset ? ` · ${alert.asset}` : ''}{alert.detail && <small className="alerts-table__detail">{alert.detail}</small>}</td>
            <td>{alert.service}</td>
            <td>{alert.timestamp || '—'}</td>
            <td>{STATUS_LABEL[alert.status]}{alert.status === 'ACKNOWLEDGED' && alert.acknowledgedBy ? ` by ${alert.acknowledgedBy}` : ''}</td>
            {ackAvailable && <td>{alert.status === 'ACTIVE' && alert.acknowledgementKey && isOperator &&
              <button className="button-secondary" onClick={() => void acknowledge(alert.id, alert.acknowledgementKey)} disabled={ackBusyId === alert.id}>{ackBusyId === alert.id ? 'Saving…' : 'Mark as seen'}</button>}</td>}
          </tr>)}
          {alerts.length === 0 && <tr><td colSpan={ackAvailable ? 6 : 5} className="empty-cell">{open.length === 0 ? 'No open alerts' : 'No alerts match the filters'}</td></tr>}
        </tbody>
      </table></div>
    </Panel>

    {history.length > 0 && <Panel title="Alert History">
      <div className="table-wrap"><table className="wide-table">
        <thead><tr><th>Time</th><th>Change</th><th>Severity</th><th>Alert</th><th>Area</th></tr></thead>
        <tbody>{history.slice(0, PAGE).map(event => <tr key={event.eventId}>
          <td>{event.recordedAt}</td>
          <td>{event.transition === 'OPENED' ? 'Opened' : event.transition === 'RESOLVED' ? 'Resolved' : 'Updated'}</td>
          <td><SeverityPill severity={event.alert.severity}/></td>
          <td>{event.alert.title}</td>
          <td>{event.alert.service}</td>
        </tr>)}</tbody>
      </table></div>
    </Panel>}

    <Panel title="Audit Log">
      <div className="table-wrap"><table className="wide-table">
        <thead><tr><th>When</th><th>Who</th><th>Action</th><th>Result</th><th>Detail</th></tr></thead>
        <tbody>
          {data.audit.slice(0, auditLimit).map(row => <tr key={row.id} title={row.correlationId}>
            <td>{row.timestamp || '—'}</td>
            <td>{row.actor}</td>
            <td>{row.action}</td>
            <td className={row.result === 'REJECTED' ? 'negative' : row.result === 'SUCCESS' ? 'positive' : ''}>{row.result === 'SUCCESS' ? 'Done' : row.result === 'REJECTED' ? 'Rejected' : 'No change'}</td>
            <td>{row.detail || '—'}</td>
          </tr>)}
          {data.audit.length === 0 && <tr><td colSpan={5} className="empty-cell">No operator actions yet</td></tr>}
        </tbody>
      </table></div>
      {data.audit.length > auditLimit && <div className="table-more"><span className="muted">{auditLimit} of {data.audit.length}</span><button className="ghost-button" onClick={() => setAuditLimit(limit => limit + PAGE)}>Show more</button></div>}
    </Panel>
  </>
}
