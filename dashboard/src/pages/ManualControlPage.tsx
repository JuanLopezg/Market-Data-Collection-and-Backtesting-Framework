import { AlertTriangle, CheckCircle2, ChevronRight, FileCheck2, FileUp, LockKeyhole, RefreshCw, ShieldCheck, UploadCloud } from 'lucide-react'
import { useMemo, useRef, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import { useAuth } from '../providers/AuthProvider'
import { useDashboardDataSource } from '../providers/DashboardDataSourceProvider'
import type { ManualControlData, ManualControlRouteResult, ManualControlStage, ManualValidationSeverity } from '../types/dashboard'

const stages: { id: ManualControlStage; label: string; number: number }[] = [
  { id: 'UPLOAD', label: 'Upload', number: 1 },
  { id: 'VALIDATE', label: 'Validate', number: 2 },
  { id: 'PREVIEW', label: 'Preview', number: 3 },
  { id: 'RISK_CHECK', label: 'Risk boundary', number: 4 },
  { id: 'ORDER_PREVIEW', label: 'Target delta', number: 5 },
  { id: 'CONFIRM', label: 'Confirm', number: 6 },
]

function issueTone(severity: ManualValidationSeverity) {
  return severity === 'ERROR' ? 'bad' : severity === 'WARN' ? 'warn' : 'good'
}

export function ManualControlPage() {
  const { data, error, retry } = useDashboardResource('getManualControl')
  const dataSource = useDashboardDataSource()
  const { session } = useAuth()
  const isOperator = session?.user.role === 'OPERATOR'
  const [preview, setPreview] = useState<ManualControlData | null>(null)
  const [routeResult, setRouteResult] = useState<ManualControlRouteResult | null>(null)
  const [stage, setStage] = useState<ManualControlStage>('UPLOAD')
  const [filename, setFilename] = useState('manual-target.csv')
  const [csvText, setCsvText] = useState('')
  const [busy, setBusy] = useState(false)
  const [routeBusy, setRouteBusy] = useState(false)
  const [previewError, setPreviewError] = useState<string | null>(null)
  const [routeError, setRouteError] = useState<string | null>(null)
  const inputRef = useRef<HTMLInputElement>(null)

  const view = preview ?? data
  const activeIndex = useMemo(() => stages.findIndex(item => item.id === stage), [stage])

  if (error) return <DataLoadError title="Manual control unavailable" error={error} onRetry={retry}/>
  if (!view) return <div className="page-loading">Loading manual control…</div>

  const runPreview = async (nextFilename: string, csv: string) => {
    if (!isOperator || !session?.csrfToken) return
    setBusy(true)
    setPreviewError(null)
    setRouteError(null)
    setRouteResult(null)
    try {
      const result = await dataSource.previewManualControl({ filename: nextFilename, csv }, session.csrfToken)
      setFilename(nextFilename)
      setCsvText(csv)
      setPreview(result)
      setStage('VALIDATE')
    } catch (cause) {
      setPreviewError(cause instanceof Error ? cause.message : 'Manual-control preview failed.')
    } finally {
      setBusy(false)
    }
  }

  const evaluateRoute = async () => {
    if (!isOperator || !session?.csrfToken || !preview?.requestHash || !preview.currentTargetTimestamp || !csvText) return
    setRouteBusy(true)
    setRouteError(null)
    try {
      const result = await dataSource.routeManualControl({
        filename,
        csv: csvText,
        requestHash: preview.requestHash,
        referenceTargetTimestamp: preview.currentTargetTimestamp,
        confirmation: preview.confirmationPhrase ?? 'CONFIRM_MANUAL_ROUTE',
      }, session.csrfToken)
      setRouteResult(result)
    } catch (cause) {
      setRouteError(cause instanceof Error ? cause.message : 'Manual route admission failed.')
    } finally {
      setRouteBusy(false)
    }
  }

  const useDemo = () => { void runPreview('manual-target-demo.csv', view.exampleCsv) }

  const onFile = async (file?: File) => {
    if (!isOperator || !file) return
    if (file.size > 256 * 1024) {
      setPreviewError('CSV exceeds the 256 KB limit.')
      return
    }
    try {
      await runPreview(file.name, await file.text())
    } catch {
      setPreviewError('Could not read the selected CSV.')
    }
  }

  const next = () => {
    if (!isOperator) return
    if (stage === 'VALIDATE' && !view.validationPassed) return
    const nextStage = stages[Math.min(activeIndex + 1, stages.length - 1)]
    setStage(nextStage.id)
  }

  const reset = () => {
    setStage('UPLOAD')
    setPreview(null)
    setRouteResult(null)
    setPreviewError(null)
    setRouteError(null)
    setFilename('manual-target.csv')
    setCsvText('')
    if (inputRef.current) inputRef.current.value = ''
  }

  const routeBlockers = view.routeBlockers ?? []
  const recentAudits = view.recentRouteAudits ?? []

  return <>
    <div className="page-heading">
      <div><h1>Manual Portfolio Control</h1><p>Confirmed server-side admission contract that can never bypass PortfolioRisk → Planner → ExecutionState → venue adapter.</p></div>
      <div className="manual-mode"><LockKeyhole size={13}/><span>{view.mode}</span><strong>{view.exchange}</strong></div>
    </div>

    {!isOperator && <div className="manual-role-lock"><LockKeyhole size={15}/><div><strong>VIEWER access</strong><span>Manual portfolio preview and route admission require an OPERATOR session.</span></div></div>}

    <div className="manual-warning"><AlertTriangle size={15}/><div><strong>{view.contractVersion?.startsWith('step58') ? 'Step 58 · MOCK Simulation Routing' : 'Step 46 · Safe Routing Contract'}</strong><span>{view.safetyNote ?? 'Routing remains fail-closed until the normal trading-control path and private venue lifecycle are proven.'}</span></div></div>
    {previewError && <div className="manual-role-lock"><AlertTriangle size={15}/><div><strong>Preview failed</strong><span>{previewError}</span></div></div>}
    {routeError && <div className="manual-role-lock"><AlertTriangle size={15}/><div><strong>Route admission failed</strong><span>{routeError}</span></div></div>}

    <Panel title={view.contractVersion?.startsWith('step58') ? "Step 58 · Simulation Routing Contract" : "Step 46 · Routing Admission Contract"} right={<span className={view.routingContractReady ? 'positive' : 'negative'}>{view.contractVersion ?? 'UNVERSIONED'} · {view.routingContractReady ? 'READY' : 'BLOCKED'}</span>}>
      <div className="gate-list">
        <span>Contract mode<strong>{view.routingContractMode ?? '—'}</strong></span>
        <span>Durable human audit<strong>{view.humanAuditAvailable ? 'AVAILABLE' : 'UNAVAILABLE'}</strong></span>
        <span>Trading-control sink<strong>{view.tradingControlSink ?? 'UNCONFIGURED'}</strong></span>
        <span>Private auth<strong>{view.privateAuth ?? 'DEFERRED'}</strong></span>
        <span>Order lifecycle<strong>{view.orderLifecycle ?? 'DEFERRED'}</strong></span>
        <span>Actual routing<strong>{view.routeEnabled ? 'ENABLED' : 'DISABLED'}</strong></span>
      </div>
      <div className="preview-note"><ShieldCheck size={13}/><span>{view.executionBoundary ?? 'Normal pipeline only.'}</span></div>
    </Panel>

    <section className={`manual-stepper ${!isOperator ? 'manual-stepper--locked' : ''}`}>
      {stages.map((item, index) => <button key={item.id} onClick={() => isOperator && preview && index <= activeIndex ? setStage(item.id) : undefined} className={`${item.id === stage ? 'manual-step--active' : ''} ${index < activeIndex ? 'manual-step--done' : ''}`}>
        <span>{index < activeIndex ? <CheckCircle2 size={14}/> : item.number}</span><strong>{item.label}</strong>{index < stages.length - 1 && <ChevronRight size={12}/>} 
      </button>)}
    </section>

    {stage === 'UPLOAD' && <section className="manual-grid manual-grid--upload">
      <Panel title="Target Portfolio CSV" right={<span className="muted">Schema · {view.schemaLabel}</span>}>
        <div className={`upload-zone ${!isOperator ? 'upload-zone--locked' : ''}`} onClick={() => isOperator && !busy ? inputRef.current?.click() : undefined}>
          <UploadCloud size={32}/><strong>{busy ? 'Validating on server…' : 'Choose target portfolio CSV'}</strong><span>Authenticated backend parser · max {view.maxUploadSizeLabel}</span><button type="button" disabled={busy || !isOperator}>Select CSV</button>
          <input ref={inputRef} type="file" disabled={!isOperator || busy} accept=".csv,text/csv" onChange={e => void onFile(e.target.files?.[0])}/>
        </div>
        <div className="manual-actions"><button className="button-secondary" onClick={useDemo} disabled={!isOperator || busy}><FileCheck2 size={13}/> Validate current-target demo</button></div>
      </Panel>
      <Panel title="Expected Format" right={<span className="muted">Backend contract</span>}>
        <pre className="csv-preview">{view.exampleCsv}</pre>
        <div className="preview-note"><ShieldCheck size={13}/><span>The backend validates schema, duplicates, 100% weights, canonical assets, explicit symbol-registry routability and current public venue-rule coverage. Browser checks are convenience only.</span></div>
      </Panel>
    </section>}

    {stage === 'VALIDATE' && <section className="manual-grid">
      <Panel title="Backend Validation Result" right={<span className={view.validationPassed ? 'positive' : 'negative'}><FileUp size={12}/> {view.validationPassed ? 'PASS' : 'FAIL'}</span>}>
        <div className="manual-file"><FileCheck2 size={24}/><div><strong>{filename}</strong><span>Server-side parser · actor {view.auditActorLabel} · preview not persisted</span></div></div>
        <div className="validation-list">{view.validationIssues.map((issue, index) => <div className={`validation-row validation-row--${issueTone(issue.severity)}`} key={`${issue.field}-${index}`}><span>{issue.severity}</span><div><strong>{issue.field}</strong><small>{issue.message}</small></div></div>)}</div>
      </Panel>
      <Panel title="Safety Gate" right={<span className={view.validationPassed ? 'positive' : 'negative'}>{view.validationPassed ? 'VALIDATED' : 'BLOCKED'}</span>}>
        <div className="gate-list"><span>Backend validation<strong>{view.backendAuthoritative ? 'SERVER' : 'MOCK'}</strong></span><span>Exchange constraints<strong>{view.exchangeConstraintsValidated ? 'VALIDATED' : 'BLOCKED'}</strong></span><span>PortfolioRisk manual check<strong>{view.riskCheckAvailable ? 'AVAILABLE' : 'NOT WIRED'}</strong></span><span>Routing<strong>{view.routeEnabled ? 'ENABLED' : 'DISABLED'}</strong></span><span>Reference target<strong>{view.currentTargetTimestamp ?? '—'}</strong></span></div>
      </Panel>
    </section>}

    {stage === 'PREVIEW' && <Panel title="Current Approved Target vs Requested Target" right={<span className="muted">Reference-capital delta · not an order</span>}>
      <div className="table-wrap"><table className="wide-table manual-preview-table"><thead><tr><th>Asset</th><th>Current approved</th><th>Requested</th><th>Delta</th><th>Target notional delta</th><th>Status</th></tr></thead><tbody>{view.previewRows.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td>{row.currentWeightPct.toFixed(2)}%</td><td>{row.requestedWeightPct.toFixed(2)}%</td><td className={row.deltaPct > 0 ? 'positive' : row.deltaPct < 0 ? 'negative' : ''}>{row.deltaPct > 0 ? '+' : ''}{row.deltaPct.toFixed(2)}%</td><td>{row.estimatedNotionalLabel}</td><td><span className="manual-tag">TARGET</span></td></tr>)}</tbody></table></div>
    </Panel>}

    {stage === 'RISK_CHECK' && <section className="manual-grid">
      <Panel title="Manual Risk Boundary" right={<span className={view.riskCheckAvailable ? 'positive' : 'warning'}>{view.riskCheckAvailable ? 'STEP57 WIRED' : 'NOT EXECUTED'}</span>}>
        <div className="manual-authority"><ShieldCheck size={28}/><strong>PortfolioRisk remains authoritative</strong><p>{view.riskCheckAvailable ? 'The confirmed MOCK request will enter the Step57 manual PortfolioRisk transformation and production planner. Preview values remain non-authoritative until that execution-time risk check runs.' : 'The request is structurally/exchange-admission validated only. This provider cannot turn requested weights into an approved target.'}</p></div>
      </Panel>
      <Panel title="Current route blockers" right={<span className="negative">FAIL CLOSED</span>}>
        <div className="gate-list">{routeBlockers.map(blocker => <span key={blocker}>{blocker}<strong>BLOCKED</strong></span>)}</div>
      </Panel>
    </section>}

    {stage === 'ORDER_PREVIEW' && <section className="manual-grid manual-grid--orders">
      <Panel title="Target Delta Preview" right={<span className="muted">{view.routeEnabled ? 'MOCK normal pipeline only' : 'Not executable'}</span>}>
        <div className="table-wrap"><table className="wide-table"><thead><tr><th>Asset</th><th>Direction</th><th>Δ weight</th><th>Reference-capital delta</th><th>Fee</th><th>Boundary</th></tr></thead><tbody>{view.orderPreview.map(row => <tr key={row.asset}><td><strong>{row.asset}</strong></td><td className={row.action === 'BUY' ? 'positive' : row.action === 'SELL' ? 'negative' : ''}>{row.action}</td><td>{row.deltaWeightPct > 0 ? '+' : ''}{row.deltaWeightPct.toFixed(2)}%</td><td>{row.estimatedNotionalLabel}</td><td>{row.estimatedFeeLabel}</td><td>{row.note}</td></tr>)}</tbody></table></div>
      </Panel>
      <Panel title="Request Summary">
        <div className="manual-summary"><span>Target turnover<strong>{view.estimatedTurnoverLabel}</strong></span><span>Estimated fees<strong>{view.estimatedFeesLabel}</strong></span><span>Request hash<code>{view.requestHash || '—'}</code></span><span>Registry<strong>{view.registryVersion ?? '—'}</strong></span><span>Actor<strong>{view.auditActorLabel}</strong></span></div>
      </Panel>
    </section>}

    {stage === 'CONFIRM' && <section className="manual-grid">
      <Panel title="Final Confirmation / Admission" right={<span className={view.routeEnabled ? 'positive' : 'negative'}><LockKeyhole size={12}/> {view.routeEnabled ? 'MOCK routing enabled' : 'routing disabled'}</span>}>
        <div className="confirm-box"><ShieldCheck size={34}/><h2>Confirm the exact hashed request</h2><p>{view.routeEnabled ? 'The server will recompute the hash and stale-reference checks, then queue the request to the Step58 simulation trading-control sink. The runner invokes Step57 Risk → Planner → CanonicalVenueAdapter → MOCK; the browser never talks to the venue.' : 'The server will recompute the CSV hash, reject a stale reference target, and remain fail-closed.'}</p><button className="button-primary" onClick={() => void evaluateRoute()} disabled={!isOperator || routeBusy || !view.validationPassed || !view.requestHash}>{routeBusy ? 'Evaluating…' : 'Evaluate safe route admission'}</button><small>Confirmation token: {view.confirmationPhrase ?? 'CONFIRM_MANUAL_ROUTE'} · {view.routeEnabled ? 'simulation sink only · no private venue / no capital' : 'no trading command dispatched'}.</small></div>
        {routeResult && <div className="alerts-gap-note"><strong>{routeResult.status} · submitted={String(routeResult.submitted)} · audit={routeResult.auditPersisted ? 'PERSISTED' : 'FAILED'}</strong><span>{routeResult.note}</span><code>{routeResult.correlationId}</code><span>{routeResult.blockers.join(' · ')}</span></div>}
      </Panel>
      <Panel title="Durable Operator-Intent Audit" right={<span className={view.humanAuditAvailable ? 'positive' : 'negative'}>{view.humanAuditAvailable ? 'AVAILABLE' : 'UNAVAILABLE'}</span>}>
        <div className="audit-preview"><span>Action<strong>MANUAL_ROUTE_ADMISSION</strong></span><span>Mode<strong>{view.mode}</strong></span><span>Boundary<strong>{view.exchange}</strong></span><span>Request hash<code>{view.requestHash || '—'}</code></span><span>Expected result<strong>{view.routeEnabled ? 'QUEUED TO MOCK / STEP57' : 'REJECTED / NOT_SUBMITTED'}</strong></span></div>
        {recentAudits.length > 0 && <div className="table-wrap"><table className="wide-table audit-table"><thead><tr><th>Recorded</th><th>Actor</th><th>Result</th><th>Submitted</th><th>Correlation</th><th>Blockers</th></tr></thead><tbody>{recentAudits.slice(0, 8).map(row => <tr key={row.correlationId}><td>{row.recordedAt}</td><td>{row.actor}</td><td>{row.result}</td><td>{String(row.submitted)}</td><td><code>{row.correlationId}</code></td><td>{row.blockers.join(', ')}</td></tr>)}</tbody></table></div>}
      </Panel>
    </section>}

    {stage !== 'UPLOAD' && <footer className="manual-footer"><button className="button-secondary" onClick={reset}><RefreshCw size={13}/> Reset</button><span>{filename} · server admission contract · {view.routeEnabled ? 'MOCK simulation routing enabled' : 'actual routing disabled'}</span>{stage !== 'CONFIRM' && <button className="button-primary" onClick={next} disabled={!isOperator || (stage === 'VALIDATE' && !view.validationPassed)}>Continue <ChevronRight size={13}/></button>}</footer>}
  </>
}
