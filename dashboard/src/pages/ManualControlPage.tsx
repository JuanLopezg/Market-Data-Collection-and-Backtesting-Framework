import { Check, UploadCloud, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import { useAuth } from '../providers/AuthProvider'
import { useDashboardDataSource } from '../providers/DashboardDataSourceProvider'
import type { ManualControlData, ManualRequestStatus } from '../types/dashboard'

// Manual Control: upload a target portfolio CSV, preview it against the current account
// and send it. The replay executes it at its next price; the PAPER/LIVE chain executes it
// at once at the current market prices. The next daily decision then returns to the
// strategy targets. A progress bar follows the request to the exchange.

const MAX_BYTES = 256 * 1024
const POLL_MS = 2000

type Notice = { tone: 'ok' | 'bad'; text: string }

const STEPS: { stage: ManualRequestStatus['stage']; label: string }[] = [
  { stage: 'QUEUED', label: 'Sent' },
  { stage: 'EXECUTED', label: 'Checked' },
  { stage: 'AT_EXCHANGE', label: 'Orders at exchange' },
  { stage: 'FILLED', label: 'Filled' },
  { stage: 'RESYNCED', label: 'Back to strategy targets' },
]
const STEP_INDEX: Record<string, number> = Object.fromEntries(STEPS.map((step, index) => [step.stage, index]))

// Wording per runtime: the replay simulation, PAPER (simulated account) or LIVE (Kraken).
function modeText(mode: string) {
  if (mode === 'REPLAY') return {
    checked: 'Picked up by replay', waiting: 'Waiting for the next replay price', send: 'Send to replay',
    timing: 'Executes at the next price; the end-of-day decision then returns to the strategy targets.',
  }
  if (mode === 'LIVE') return {
    checked: 'Checked by execution', waiting: 'Waiting for execution-state (a few seconds)', send: 'Send real orders to Kraken',
    timing: 'Executes now at the market on Kraken; the 00:00 UTC daily cycle then returns to the strategy targets. No requests from 23:45 to 00:15 UTC.',
  }
  return {
    checked: 'Checked by execution', waiting: 'Waiting for execution-state (a few seconds)', send: 'Send (PAPER)',
    timing: 'Executes now at the current Binance prices in the simulated account; the 00:00 UTC daily cycle then returns to the strategy targets. No requests from 23:45 to 00:15 UTC.',
  }
}

const pct = (value: number) => `${value.toFixed(2)}%`
const signedPct = (value: number) => `${value > 0 ? '+' : ''}${value.toFixed(2)}%`

function actionTone(action?: string) {
  return action === 'BUY' ? 'positive' : action === 'SELL' || action === 'CLOSE' ? 'negative' : 'muted'
}

function RequestProgress({ status, checkedLabel }: { status?: ManualRequestStatus; checkedLabel: string }) {
  const failed = status?.stage === 'FAILED'
  const reached = status ? (failed ? 0 : STEP_INDEX[status.stage]) : 0
  return <div className="manual-progress">
    <ol>{STEPS.map((step, index) => {
      const state = failed && index === 1 ? 'failed' : index <= reached ? 'done' : index === reached + 1 && !failed ? 'current' : 'todo'
      const base = step.stage === 'EXECUTED' ? checkedLabel : step.label
      const label = step.stage === 'AT_EXCHANGE' && status && status.orders > 0 ? `${base} (${status.filled}/${status.orders} filled)` : base
      return <li key={step.stage} className={`manual-progress__step manual-progress__step--${state}`}>
        <span>{state === 'done' ? <Check size={12}/> : state === 'failed' ? <X size={12}/> : index + 1}</span>{failed && index === 1 ? 'Rejected' : label}
      </li>
    })}</ol>
    {status?.detail && <small className={failed ? 'negative' : 'muted'}>{status.detail}</small>}
  </div>
}

export function ManualControlPage() {
  const { data, error, retry } = useDashboardResource('getManualControl')
  const dataSource = useDashboardDataSource()
  const { session } = useAuth()
  const isOperator = session?.user.role === 'OPERATOR'
  const [preview, setPreview] = useState<ManualControlData | null>(null)
  const [file, setFile] = useState<{ name: string; csv: string } | null>(null)
  const [busy, setBusy] = useState(false)
  const [notice, setNotice] = useState<Notice | null>(null)
  const [trackedId, setTrackedId] = useState<string | null>(null)
  const [dragging, setDragging] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)

  const tracked = data?.requests?.find(request => request.id === trackedId)
  const finished = tracked?.stage === 'RESYNCED' || tracked?.stage === 'FAILED'

  // Poll while a sent request is still on its way.
  useEffect(() => {
    if (!trackedId || finished) return
    const timer = window.setInterval(retry, POLL_MS)
    return () => window.clearInterval(timer)
  }, [trackedId, finished, retry])

  if (error) return <DataLoadError title="Manual control unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading manual control…</div>

  const canSend = isOperator && !!session?.csrfToken

  const runPreview = async (name: string, csv: string) => {
    if (!canSend) return
    setBusy(true)
    setNotice(null)
    try {
      setPreview(await dataSource.previewManualControl({ filename: name, csv }, session!.csrfToken))
      setFile({ name, csv })
    } catch (cause) {
      setNotice({ tone: 'bad', text: cause instanceof Error ? cause.message : 'Preview failed.' })
    } finally {
      setBusy(false)
    }
  }

  const onFile = async (selected?: File) => {
    if (!selected) return
    if (selected.size > MAX_BYTES) {
      setNotice({ tone: 'bad', text: 'The CSV is larger than 256 KB.' })
      return
    }
    await runPreview(selected.name, await selected.text())
  }

  const clear = () => {
    setPreview(null)
    setFile(null)
    if (inputRef.current) inputRef.current.value = ''
  }

  const send = async () => {
    if (!canSend || !preview || !file) return
    setBusy(true)
    try {
      const result = await dataSource.routeManualControl({
        filename: file.name, csv: file.csv, requestHash: preview.requestHash,
        referenceTargetTimestamp: preview.currentTargetTimestamp ?? '',
        confirmation: preview.confirmationPhrase ?? 'CONFIRM_MANUAL_ROUTE',
      }, session!.csrfToken)
      if (result.status === 'QUEUED') {
        setTrackedId(result.correlationId)
        setNotice(null)
        clear()
        retry()
      } else {
        setNotice({ tone: 'bad', text: `Not sent: ${result.blockers.join(', ') || result.note}` })
      }
    } catch (cause) {
      setNotice({ tone: 'bad', text: cause instanceof Error ? cause.message : 'Sending failed.' })
    } finally {
      setBusy(false)
    }
  }

  const errors = preview?.validationIssues.filter(issue => issue.severity !== 'INFO') ?? []
  const summary = preview?.validationIssues.find(issue => issue.severity === 'INFO')
  const blockers = (preview?.routeBlockers ?? []).filter(item => item !== 'PREVIEW_VALIDATION_FAILED')
  const sendable = !!preview?.validationPassed && !!preview.routeEnabled && !busy
  const grossPct = data.maxGrossPct ?? 100
  const assetPct = data.maxAssetPct ?? 100
  const text = modeText(data.mode)
  const live = data.mode === 'LIVE'

  return <>
    <div className="page-heading"><h1>Manual Control</h1></div>

    {live && <div className="manual-banner manual-banner--bad">LIVE: sending a portfolio places real orders on Kraken with real money.</div>}
    {!isOperator && <div className="manual-banner">Sign in as an OPERATOR to send a portfolio.</div>}
    {notice && <div className={`manual-banner manual-banner--${notice.tone}`}>{notice.text}</div>}

    {trackedId && <Panel title="Request Progress" right={finished ? <button className="button-secondary" onClick={() => setTrackedId(null)}>Close</button> : undefined}>
      <RequestProgress checkedLabel={text.checked} status={tracked ?? { id: trackedId, stage: 'QUEUED', detail: text.waiting, orders: 0, filled: 0 }}/>
    </Panel>}

    {!preview && <section className="manual-layout">
      <Panel title="Upload Target Portfolio">
        <div
          className={`upload-zone ${!canSend ? 'upload-zone--locked' : ''} ${dragging ? 'upload-zone--drag' : ''}`}
          onClick={() => canSend && !busy ? inputRef.current?.click() : undefined}
          onDragOver={event => { event.preventDefault(); if (canSend) setDragging(true) }}
          onDragLeave={() => setDragging(false)}
          onDrop={event => { event.preventDefault(); setDragging(false); if (canSend && !busy) void onFile(event.dataTransfer.files?.[0]) }}>
          <UploadCloud size={30}/>
          <strong>{busy ? 'Checking…' : 'Drop a CSV here or click to choose'}</strong>
          <span>Max 256 KB</span>
          <input ref={inputRef} type="file" disabled={!canSend || busy} accept=".csv,text/csv" onChange={event => void onFile(event.target.files?.[0])}/>
        </div>
      </Panel>
      <Panel title="Format" right={<button className="button-secondary" onClick={() => void runPreview('example.csv', data.exampleCsv)} disabled={!canSend || busy}>Try example</button>}>
        <pre className="csv-preview">{data.exampleCsv}</pre>
        <ul className="manual-rules">
          <li><code>weight_pct</code> is the % of equity per asset.</li>
          <li>CASH is optional; whatever is not allocated stays in cash.</li>
          <li>Held assets missing from the file are closed.</li>
          <li>Risk limits: total up to {grossPct.toFixed(0)}% ({(grossPct / 100).toFixed(2)}x), {assetPct.toFixed(0)}% per asset, long only.</li>
          <li>{text.timing}</li>
        </ul>
      </Panel>
    </section>}

    {preview && <Panel title={`Preview · ${file?.name ?? ''}`} right={<span className={preview.validationPassed ? 'positive' : 'negative'}>{preview.validationPassed ? 'Valid' : 'Invalid'}</span>}>
      {errors.length > 0 && <ul className="manual-issues">{errors.map((issue, index) => <li key={index} className={issue.severity === 'ERROR' ? 'negative' : 'warning'}><strong>{issue.field}</strong> {issue.message}</li>)}</ul>}
      {preview.validationPassed && <>
        <div className="table-wrap"><table className="wide-table">
          <thead><tr><th>Asset</th><th>Action</th><th>Current</th><th>Target</th><th>Change</th><th>Approx. Amount</th></tr></thead>
          <tbody>{preview.previewRows.map(row => <tr key={row.asset}>
            <td><strong>{row.asset}</strong></td>
            <td className={actionTone(row.action)}>{row.action ?? '—'}</td>
            <td>{pct(row.currentWeightPct)}</td>
            <td>{pct(row.requestedWeightPct)}</td>
            <td className={row.deltaPct > 0 ? 'positive' : row.deltaPct < 0 ? 'negative' : ''}>{signedPct(row.deltaPct)}</td>
            <td>{row.estimatedNotionalLabel}</td>
          </tr>)}</tbody>
        </table></div>
        <div className="manual-summary-line">{summary?.message} Turnover {preview.estimatedTurnoverLabel}. Amounts use the current equity; the planner sizes the final orders at the execution price.</div>
      </>}
      {blockers.length > 0 && <div className="manual-banner manual-banner--bad">Cannot send now: {blockers.join(', ')}</div>}
      <div className="manual-actions">
        <button className="button-secondary" onClick={clear} disabled={busy}>Cancel</button>
        <button className="button-primary" onClick={() => void send()} disabled={!sendable}>{busy ? 'Sending…' : text.send}</button>
      </div>
    </Panel>}
  </>
}
