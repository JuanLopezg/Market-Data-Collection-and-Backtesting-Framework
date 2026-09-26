import { Activity, Box, CheckCircle2, Clock3, Database, HardDrive, Network, Radio, Server, TriangleAlert, Wifi } from 'lucide-react'
import { Panel } from '../components/Panel'
import { DataLoadError } from '../components/DataLoadError'
import { useDashboardResource } from '../hooks/useDashboardResource'
import type { InfrastructureData, InfrastructureHealth } from '../types/dashboard'

function HealthPill({ state }: { state: InfrastructureHealth }) {
  const tone = state === 'HEALTHY' ? 'good' : state === 'WARN' ? 'warn' : state === 'CRITICAL' ? 'bad' : 'muted'
  return <span className={`infra-state infra-state--${tone}`}>{state}</span>
}

function Meter({ value }: { value: number }) {
  const tone = value >= 90 ? 'bad' : value >= 75 ? 'warn' : 'good'
  return <div className="infra-meter"><div><span className={`infra-meter__fill infra-meter__fill--${tone}`} style={{ width: `${Math.min(100, value)}%` }}/></div><strong>{value.toFixed(0)}%</strong></div>
}

function formatBytes(value: number) {
  if (!Number.isFinite(value) || value < 0) return '—'
  const mb = value / (1024 * 1024)
  return `${mb.toFixed(mb >= 100 ? 0 : 1)} MB`
}

export function InfrastructurePage() {
  const { data, error, retry } = useDashboardResource('getInfrastructure')
  const { data: diagnostics, error: diagnosticsError } = useDashboardResource('getDiagnostics')
  const { data: safetyGate, error: safetyGateError } = useDashboardResource('getSafetyGate')
  const { data: globalReadiness, error: globalReadinessError } = useDashboardResource('getGlobalReadiness')
  const { data: venueFoundation, error: venueFoundationError } = useDashboardResource('getVenueFoundation')
  const { data: venuePublic, error: venuePublicError } = useDashboardResource('getVenuePublic')
  const { data: venueSymbolMap, error: venueSymbolMapError } = useDashboardResource('getVenueSymbolMapping')
  const { data: venueRules, error: venueRulesError } = useDashboardResource('getVenueTradingRules')
  const { data: symbolRegistry, error: symbolRegistryError } = useDashboardResource('getSymbolRegistry')

  if (error) return <DataLoadError title="Infrastructure data unavailable" error={error} onRetry={retry}/>
  if (!data) return <div className="page-loading">Loading infrastructure…</div>

  const vpsObserved = data.vps.state !== 'UNKNOWN'
  const metric = (value: number) => value < 0 ? '—' : value.toString()

  return <>
    <div className="page-heading">
      <div><h1>Infrastructure</h1><p>VPS, containers, persistence, messaging, trading services and exchange connectivity.</p></div>
      <div className={`readiness-chip readiness-chip--${data.readiness.toLowerCase()}`}><CheckCircle2 size={14}/><span>TRADING {data.readiness}</span></div>
    </div>

    {data.sourceMode && <div className="real-source-banner"><strong>{data.sourceMode} DATA</strong><span>{data.sourceNote}</span></div>}

    <section className="infra-summary">
      <div className="mini-metric"><span>CPU</span><strong>{vpsObserved ? `${data.vps.cpuPct.toFixed(0)}%` : '—'}</strong>{vpsObserved ? <Meter value={data.vps.cpuPct}/> : <small>Not wired</small>}</div>
      <div className="mini-metric"><span>RAM</span><strong>{vpsObserved ? `${data.vps.ramPct.toFixed(0)}%` : '—'}</strong>{vpsObserved ? <Meter value={data.vps.ramPct}/> : <small>Not wired</small>}</div>
      <div className="mini-metric"><span>Disk</span><strong>{vpsObserved ? `${data.vps.diskPct.toFixed(0)}%` : '—'}</strong>{vpsObserved ? <Meter value={data.vps.diskPct}/> : <small>Not wired</small>}</div>
      <div className="mini-metric"><span>Clock sync</span><strong className={!vpsObserved ? 'muted' : data.vps.clockSynced ? 'positive' : 'negative'}>{!vpsObserved ? 'UNKNOWN' : data.vps.clockSynced ? 'SYNCED' : 'DRIFT'}</strong><small>{data.vps.clockOffsetLabel}</small></div>
      <div className="mini-metric"><span>Uptime</span><strong>{data.vps.uptimeLabel}</strong><small>{vpsObserved ? `Load ${data.vps.load1m.toFixed(2)}` : 'Host metrics not wired'}</small></div>
    </section>

    <Panel title="Step 32 · Pre-Testnet Safety Gate" right={<span className={safetyGate?.status === 'BLOCKED' ? 'negative' : safetyGate?.status === 'WARN' ? 'warning' : safetyGate ? 'positive' : 'muted'}>{safetyGate ? safetyGate.status : safetyGateError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {safetyGate ? <>
        <div className="infra-detail-grid">
          <div><span>Safe to continue integration</span><strong className={safetyGate.safeToProceed ? 'positive' : 'negative'}>{safetyGate.safeToProceed ? 'YES' : 'NO'}</strong></div>
          <div><span>Trading ready</span><strong className={safetyGate.tradingReady ? 'positive' : 'warning'}>{safetyGate.tradingReady ? 'YES' : 'NO · FAIL-CLOSED'}</strong></div>
          <div><span>Runtime readiness</span><strong>{safetyGate.runtimeReadiness}</strong></div>
          <div><span>Step 31 proof</span><strong>{safetyGate.endToEndProofStatus} · {safetyGate.endToEndCompletion}</strong></div>
        </div>
        <div className="dependency-list">{safetyGate.checks.map(check => <div className="dependency-row" key={check.id}><div className="dependency-icon">{check.state === 'PASS' ? <CheckCircle2 size={14}/> : <TriangleAlert size={14}/>}</div><div><strong>{check.label}</strong><small>{check.detail}</small></div><span>{check.state}</span><HealthPill state={check.state === 'PASS' ? 'HEALTHY' : check.state === 'WARN' ? 'WARN' : 'CRITICAL'}/></div>)}</div>
        <div className="muted" style={{marginTop: 10}}>{safetyGate.note}</div>
      </> : <div className="muted">{safetyGateError ? 'Safety gate unavailable. Do not infer readiness from the UI.' : 'Evaluating read-only runtime evidence…'}</div>}
    </Panel>

    <Panel title="Step 44 · Full Global Readiness Contract" right={<span className={globalReadiness?.safeToContinueDashboard ? 'positive' : globalReadinessError ? 'warning' : globalReadiness ? 'negative' : 'muted'}>{globalReadiness ? globalReadiness.status : globalReadinessError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {globalReadiness ? <>
        <div className="infra-detail-grid">
          <div><span>Contract</span><strong>{globalReadiness.contractVersion} · {globalReadiness.contractComplete ? 'COMPLETE' : 'INCOMPLETE'}</strong></div>
          <div><span>Continue dashboard</span><strong className={globalReadiness.safeToContinueDashboard ? 'positive' : 'negative'}>{globalReadiness.safeToContinueDashboard ? 'YES' : 'NO'}</strong></div>
          <div><span>Private testnet ready</span><strong className={globalReadiness.privateTestnetReady ? 'positive' : 'warning'}>{globalReadiness.privateTestnetReady ? 'YES' : 'NO · DEFERRED'}</strong></div>
          <div><span>Trading ready</span><strong className={globalReadiness.tradingReady ? 'positive' : 'warning'}>{globalReadiness.tradingReady ? 'YES' : 'NO · FAIL-CLOSED'}</strong></div>
          <div><span>LIVE ready</span><strong className={globalReadiness.liveReady ? 'positive' : 'warning'}>{globalReadiness.liveReady ? 'YES' : 'NO · FAIL-CLOSED'}</strong></div>
          <div><span>Routing</span><strong>{globalReadiness.orderRouting} · manual {globalReadiness.manualRouting}</strong></div>
          <div><span>Warnings / deferred</span><strong>{globalReadiness.warningCount} / {globalReadiness.deferredCount}</strong></div>
          <div><span>Next safe step</span><strong>{globalReadiness.nextSafeStep}</strong></div>
        </div>
        <div className="dependency-list">{globalReadiness.requirements.map(req => <div className="dependency-row" key={req.id}><div className="dependency-icon">{req.state === 'PASS' ? <CheckCircle2 size={14}/> : <TriangleAlert size={14}/>}</div><div><strong>{req.label}</strong><small>{req.detail}</small></div><span>{req.requiredNow ? 'NOW' : req.requiredFor.join(' / ')}</span><HealthPill state={req.state === 'PASS' ? 'HEALTHY' : req.state === 'BLOCKED' ? 'CRITICAL' : req.state === 'WARN' ? 'WARN' : 'UNKNOWN'}/></div>)}</div>
        <div className="muted" style={{marginTop: 10}}>{globalReadiness.note}</div>
        <div className="muted" style={{marginTop: 6}}><strong>Future replay boundary:</strong> {globalReadiness.futureReplayBoundary}</div>
      </> : <div className="muted">{globalReadinessError ? 'Global readiness contract unavailable. No readiness must be inferred from missing evidence.' : 'Evaluating the explicit global readiness contract…'}</div>}
    </Panel>

    <Panel title="Step 33 · Testnet Integration Foundation" right={<span className={venueFoundation?.foundationReady ? 'positive' : venueFoundationError ? 'warning' : 'muted'}>{venueFoundation ? venueFoundation.status : venueFoundationError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {venueFoundation ? <>
        <div className="infra-detail-grid">
          <div><span>Execution venue</span><strong>{venueFoundation.venue}</strong></div>
          <div><span>Target environment</span><strong>{venueFoundation.targetEnvironment}</strong></div>
          <div><span>Gateway mode</span><strong>{venueFoundation.gatewayMode}</strong></div>
          <div><span>Public connectivity</span><strong className="warning">{venueFoundation.publicConnectivity}</strong></div>
          <div><span>Private auth</span><strong className={venueFoundation.privateAuth === 'DISABLED' ? 'positive' : 'negative'}>{venueFoundation.privateAuth}</strong></div>
          <div><span>Order routing</span><strong className={venueFoundation.orderRouting === 'DISABLED' ? 'positive' : 'negative'}>{venueFoundation.orderRouting}</strong></div>
          <div><span>Symbol mapping</span><strong>{venueFoundation.symbolMapping}</strong></div>
          <div><span>Exchange filters</span><strong>{venueFoundation.exchangeFilters}</strong></div>
          <div><span>Secrets required</span><strong className={!venueFoundation.secretsRequired ? 'positive' : 'negative'}>{venueFoundation.secretsRequired ? 'YES' : 'NO'}</strong></div>
          <div><span>Real capital required</span><strong className={!venueFoundation.capitalRequired ? 'positive' : 'negative'}>{venueFoundation.capitalRequired ? 'YES' : 'NO'}</strong></div>
        </div>
        <div className="muted" style={{marginTop: 10}}><strong>Audited boundary:</strong> {venueFoundation.sourceContract}</div>
        <div className="muted" style={{marginTop: 6}}>{venueFoundation.note}</div>
      </> : <div className="muted">{venueFoundationError ? 'Venue foundation endpoint unavailable. No exchange readiness should be inferred.' : 'Loading fail-closed venue foundation…'}</div>}
    </Panel>

    <Panel title="Step 34 · Public Venue Connectivity / Metadata" right={<span className={venuePublic?.connected ? 'positive' : venuePublicError || venuePublic?.error ? 'negative' : 'muted'}>{venuePublic ? venuePublic.status : venuePublicError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {venuePublic ? <>
        <div className="infra-detail-grid">
          <div><Wifi size={14}/><span>Public API</span><strong className={venuePublic.connected ? 'positive' : 'negative'}>{venuePublic.connected ? 'CONNECTED' : 'BLOCKED'}</strong></div>
          <div><span>Environment</span><strong>{venuePublic.venue} · {venuePublic.targetEnvironment}</strong></div>
          <div><span>Metadata</span><strong className={venuePublic.metadataStatus === 'AVAILABLE' ? 'positive' : 'negative'}>{venuePublic.metadataStatus}</strong></div>
          <div><span>Public mids</span><strong className={venuePublic.midsStatus === 'AVAILABLE' ? 'positive' : 'negative'}>{venuePublic.midsStatus}</strong></div>
          <div><span>Perp universe</span><strong>{venuePublic.universeCount}</strong></div>
          <div><span>Public mids count</span><strong>{venuePublic.midCount}</strong></div>
          <div><span>Metadata/mid overlap</span><strong>{venuePublic.matchedMidCount}</strong></div>
          <div><Activity size={14}/><span>Probe latency</span><strong>{venuePublic.latencyMs} ms</strong></div>
          <div><span>Private auth</span><strong className="positive">{venuePublic.privateAuth}</strong></div>
          <div><span>Order routing</span><strong className="positive">{venuePublic.orderRouting}</strong></div>
          <div><span>Symbol mapping</span><strong>{venuePublic.symbolMapping}</strong></div>
          <div><span>Venue rules</span><strong>{venuePublic.exchangeFilters}</strong></div>
        </div>
        {venuePublic.sampleSymbols.length > 0 && <div className="muted" style={{marginTop: 10}}><strong>Observed public symbols:</strong> {venuePublic.sampleSymbols.join(', ')}</div>}
        {venuePublic.error && <div className="negative" style={{marginTop: 8}}><strong>Probe error:</strong> {venuePublic.error}</div>}
        <div className="muted" style={{marginTop: 8}}><strong>Endpoint:</strong> {venuePublic.endpoint || 'Not configured'} · checked {venuePublic.checkedAt}</div>
        <div className="muted" style={{marginTop: 6}}>{venuePublic.note}</div>
      </> : <div className="muted">{venuePublicError ? 'Public venue probe unavailable. Do not infer exchange connectivity.' : 'Checking Hyperliquid TESTNET public metadata…'}</div>}
    </Panel>

    <Panel title="Step 35 · Explicit Symbol Mapping" right={<span className={venueSymbolMap?.validated ? 'positive' : venueSymbolMapError || venueSymbolMap?.error ? 'negative' : 'muted'}>{venueSymbolMap ? venueSymbolMap.status : venueSymbolMapError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {venueSymbolMap ? <>
        <div className="infra-detail-grid">
          <div><span>Policy</span><strong>{venueSymbolMap.policy}</strong></div>
          <div><span>Environment</span><strong>{venueSymbolMap.venue} · {venueSymbolMap.targetEnvironment}</strong></div>
          <div><span>Required strategy symbols</span><strong>{venueSymbolMap.requiredSymbolCount}</strong></div>
          <div><span>Classified symbols</span><strong className={venueSymbolMap.validated ? 'positive' : 'warning'}>{venueSymbolMap.classifiedRequiredCount}/{venueSymbolMap.requiredSymbolCount}</strong></div>
          <div><span>Supported mappings</span><strong>{venueSymbolMap.mappedRequiredCount}</strong></div>
          <div><span>Blocked / unsupported</span><strong className={venueSymbolMap.unsupportedRequiredCount > 0 ? 'warning' : 'positive'}>{venueSymbolMap.unsupportedRequiredCount}</strong></div>
          <div><span>Execution coverage</span><strong className={venueSymbolMap.executionCoverageComplete ? 'positive' : 'warning'}>{venueSymbolMap.executionCoverageComplete ? 'COMPLETE' : 'PARTIAL · FAIL-CLOSED'}</strong></div>
          <div><span>Classification catalog</span><strong>{venueSymbolMap.classificationEntryCount} ({venueSymbolMap.mappingEntryCount} mapped + {venueSymbolMap.unsupportedCatalogEntryCount} blocked)</strong></div>
          <div><Activity size={14}/><span>Metadata latency</span><strong>{venueSymbolMap.metadataLatencyMs} ms</strong></div>
          <div><span>Private auth</span><strong className="positive">{venueSymbolMap.privateAuth}</strong></div>
          <div><span>Order routing</span><strong className="positive">{venueSymbolMap.orderRouting}</strong></div>
          <div><span>Venue rules</span><strong>{venueSymbolMap.exchangeFilters}</strong></div>
          <div><span>Read-only</span><strong className={venueSymbolMap.readOnly ? 'positive' : 'negative'}>{venueSymbolMap.readOnly ? 'YES' : 'NO'}</strong></div>
        </div>
        {venueSymbolMap.missingInternalSymbols.length > 0 && <div className="negative" style={{marginTop: 10}}><strong>Unclassified symbols — BLOCKED:</strong> {venueSymbolMap.missingInternalSymbols.join(', ')}</div>}
        {venueSymbolMap.unsupportedInternalSymbols.length > 0 && <div className="warning" style={{marginTop: 10}}><strong>Explicitly non-routable in current TESTNET coverage:</strong> {venueSymbolMap.unsupportedInternalSymbols.join(', ')}</div>}
        {venueSymbolMap.missingVenueSymbols.length > 0 && <div className="warning" style={{marginTop: 6}}><strong>Mapped venue coins absent from current TESTNET metadata:</strong> {venueSymbolMap.missingVenueSymbols.join(', ')}</div>}
        {venueSymbolMap.rows.length > 0 && <div className="table-wrap" style={{marginTop: 10}}><table><thead><tr><th>Internal symbol</th><th>Hyperliquid coin</th><th>State</th><th>Policy / reason</th></tr></thead><tbody>{venueSymbolMap.rows.map(row => <tr key={row.internal}><td><strong>{row.internal}</strong></td><td>{row.venue || '—'}</td><td className={row.state === 'VALID' ? 'positive' : row.state.startsWith('UNSUPPORTED_') ? 'warning' : 'negative'}>{row.state}</td><td>{row.reason || 'Explicit mapping present in current TESTNET metadata.'}</td></tr>)}</tbody></table></div>}
        {venueSymbolMap.error && <div className="negative" style={{marginTop: 8}}><strong>Validation error:</strong> {venueSymbolMap.error}</div>}
        <div className="muted" style={{marginTop: 8}}><strong>Mapping artifact:</strong> {venueSymbolMap.mappingArtifactSha256 ? venueSymbolMap.mappingArtifactSha256.slice(0, 16) + '…' : 'Unavailable'} · checked {venueSymbolMap.checkedAt}</div>
        <div className="muted" style={{marginTop: 6}}>{venueSymbolMap.note}</div>
      </> : <div className="muted">{venueSymbolMapError ? 'Symbol mapping validation unavailable. Do not infer venue compatibility.' : 'Validating exact internal → venue symbols…'}</div>}
    </Panel>

    <Panel title="Step 36 · Public Trading Rules / Precision" right={<span className={venueRules?.validated ? 'positive' : venueRulesError || venueRules?.error ? 'negative' : 'muted'}>{venueRules ? venueRules.status : venueRulesError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {venueRules ? <>
        <div className="infra-detail-grid">
          <div><span>Supported mappings checked</span><strong className={venueRules.validated ? 'positive' : 'warning'}>{venueRules.validatedRuleCount}/{venueRules.supportedMappingCount}</strong></div>
          <div><span>Step 35 non-routable</span><strong className={venueRules.nonRoutableMappingCount > 0 ? 'warning' : 'positive'}>{venueRules.nonRoutableMappingCount}</strong></div>
          <div><span>Blocked rule rows</span><strong className={venueRules.blockedRuleCount > 0 ? 'negative' : 'positive'}>{venueRules.blockedRuleCount}</strong></div>
          <div><span>Minimum order notional</span><strong>${venueRules.minOrderNotionalUsd.toFixed(2)} USD</strong></div>
          <div><span>Price significant figures</span><strong>{venueRules.priceMaxSignificantFigures}</strong></div>
          <div><span>Perp MAX_DECIMALS</span><strong>{venueRules.perpMaxDecimals}</strong></div>
          <div><span>Integer price exception</span><strong className={venueRules.integerPricesAlwaysAllowed ? 'positive' : 'negative'}>{venueRules.integerPricesAlwaysAllowed ? 'ALLOWED' : 'NO'}</strong></div>
          <div><Activity size={14}/><span>Metadata/mids latency</span><strong>{venueRules.metadataLatencyMs} ms</strong></div>
          <div><span>Private auth</span><strong className="positive">{venueRules.privateAuth}</strong></div>
          <div><span>Order routing</span><strong className="positive">{venueRules.orderRouting}</strong></div>
          <div><span>Read-only</span><strong className={venueRules.readOnly ? 'positive' : 'negative'}>{venueRules.readOnly ? 'YES' : 'NO'}</strong></div>
        </div>
        {venueRules.rows.length > 0 && <div className="table-wrap" style={{marginTop: 10}}><table className="wide-table"><thead><tr><th>Internal</th><th>Venue</th><th>State</th><th>szDecimals</th><th>Size step</th><th>Price rule</th><th>Max lev.</th><th>Margin</th><th>Mid</th><th>≈ min size @ $10</th></tr></thead><tbody>{venueRules.rows.map(row => <tr key={row.internal}><td><strong>{row.internal}</strong></td><td>{row.venue}</td><td className={row.state === 'VALID' ? 'positive' : 'negative'}>{row.state}</td><td>{row.sizeDecimals}</td><td>{row.sizeStepLabel || '—'}</td><td>≤{row.priceMaxSignificantFigures} sig figs · ≤{row.priceMaxDecimals} dp · integer px allowed</td><td>{row.maxLeverage > 0 ? `${row.maxLeverage}x` : '—'}</td><td>{row.onlyIsolated ? `ISOLATED${row.marginMode ? ` · ${row.marginMode}` : ''}` : row.marginMode || 'default'}</td><td>{row.midPrice || '—'}</td><td>{row.estimatedMinOrderSizeLabel || '—'}</td></tr>)}</tbody></table></div>}
        {venueRules.rows.some(row => row.state !== 'VALID') && <div className="negative" style={{marginTop: 10}}><strong>Rule blocker:</strong> {venueRules.rows.filter(row => row.state !== 'VALID').map(row => `${row.internal}: ${row.reason || row.state}`).join(' · ')}</div>}
        {venueRules.error && <div className="negative" style={{marginTop: 8}}><strong>Validation error:</strong> {venueRules.error}</div>}
        <div className="muted" style={{marginTop: 8}}><strong>Rule source:</strong> {venueRules.ruleSource} · checked {venueRules.checkedAt}</div>
        <div className="muted" style={{marginTop: 6}}>{venueRules.note}</div>
      </> : <div className="muted">{venueRulesError ? 'Public venue trading-rule validation unavailable. Order routing must remain disabled.' : 'Validating public precision/leverage/min-notional rules for supported mappings…'}</div>}
    </Panel>

    <Panel title="Step 37A · Multi-Exchange Symbol Registry + Drift Alarms" right={<span className={symbolRegistry?.status === 'BLOCKED' ? 'negative' : symbolRegistry?.validated ? 'positive' : symbolRegistryError ? 'negative' : 'muted'}>{symbolRegistry ? symbolRegistry.status : symbolRegistryError ? 'UNAVAILABLE' : 'CHECKING'}</span>}>
      {symbolRegistry ? <>
        <div className="infra-detail-grid">
          <div><span>Registry version</span><strong>{symbolRegistry.registryVersion}</strong></div>
          <div><span>Registry entries</span><strong>{symbolRegistry.registryEntryCount}</strong></div>
          <div><span>Market-data source</span><strong>{symbolRegistry.marketDataSource}</strong></div>
          <div><span>Execution venue</span><strong>{symbolRegistry.executionVenue} · {symbolRegistry.executionEnvironment}</strong></div>
          <div><span>Current ranking registered</span><strong className={symbolRegistry.currentRankingRegisteredCount === symbolRegistry.currentRankingCount ? 'positive' : 'negative'}>{symbolRegistry.currentRankingRegisteredCount}/{symbolRegistry.currentRankingCount}</strong></div>
          <div><span>Current strategy registered</span><strong className={symbolRegistry.currentStrategyRegisteredCount === symbolRegistry.currentStrategyCount ? 'positive' : 'negative'}>{symbolRegistry.currentStrategyRegisteredCount}/{symbolRegistry.currentStrategyCount}</strong></div>
          <div><span>Current strategy routable</span><strong className={symbolRegistry.executionCoverageComplete ? 'positive' : 'warning'}>{symbolRegistry.currentStrategyRoutableCount}/{symbolRegistry.currentStrategyCount}</strong></div>
          <div><span>Non-routable current strategy</span><strong className={symbolRegistry.currentStrategyNonRoutableCount > 0 ? 'warning' : 'positive'}>{symbolRegistry.currentStrategyNonRoutableCount}</strong></div>
          <div><span>Registry alarms</span><strong className={symbolRegistry.alarms.some(a => a.severity === 'CRITICAL') ? 'negative' : symbolRegistry.alarms.length > 0 ? 'warning' : 'positive'}>{symbolRegistry.alarms.length}</strong></div>
          <div><span>Private auth</span><strong className="warning">{symbolRegistry.privateAuth}</strong></div>
          <div><span>Order routing</span><strong className="positive">{symbolRegistry.orderRouting}</strong></div>
          <div><span>Read-only</span><strong className={symbolRegistry.readOnly ? 'positive' : 'negative'}>{symbolRegistry.readOnly ? 'YES' : 'NO'}</strong></div>
        </div>
        {symbolRegistry.unregisteredRankingSymbols.length > 0 && <div className="negative" style={{marginTop: 10}}><strong>CRITICAL · unregistered Binance/source symbols:</strong> {symbolRegistry.unregisteredRankingSymbols.join(', ')}</div>}
        {symbolRegistry.mappingDivergences.length > 0 && <div className="negative" style={{marginTop: 6}}><strong>CRITICAL · registry drift:</strong> {symbolRegistry.mappingDivergences.join(' · ')}</div>}
        {symbolRegistry.venueAbsentStrategySymbols.length > 0 && <div className="warning" style={{marginTop: 6}}><strong>Venue-absent current strategy symbols:</strong> {symbolRegistry.venueAbsentStrategySymbols.join(', ')}</div>}
        {symbolRegistry.explicitBlockedStrategySymbols.length > 0 && <div className="warning" style={{marginTop: 6}}><strong>Explicitly blocked current strategy symbols:</strong> {symbolRegistry.explicitBlockedStrategySymbols.join(', ')}</div>}
        {symbolRegistry.rows.length > 0 && <div className="table-wrap" style={{marginTop: 10}}><table className="wide-table"><thead><tr><th>Internal</th><th>Binance/source</th><th>Hyperliquid TESTNET</th><th>Registry state</th><th>Runtime state</th><th>In strategy</th><th>Routing policy</th></tr></thead><tbody>{symbolRegistry.rows.map(row => <tr key={row.internal}><td><strong>{row.internal}</strong></td><td>{row.marketDataSymbol}</td><td>{row.executionSymbol || '—'}</td><td>{row.registryState}</td><td className={row.runtimeState === 'ROUTABLE_PUBLICLY_VALIDATED' ? 'positive' : row.runtimeState === 'VENUE_ABSENT' ? 'warning' : 'warning'}>{row.runtimeState}</td><td>{row.inCurrentStrategy ? 'YES' : 'NO'}</td><td>{row.routingPolicy || '—'}</td></tr>)}</tbody></table></div>}
        {symbolRegistry.error && <div className="negative" style={{marginTop: 8}}><strong>Registry validation error:</strong> {symbolRegistry.error}</div>}
        <div className="muted" style={{marginTop: 8}}><strong>Registry artifact:</strong> {symbolRegistry.registryArtifactSha256 ? symbolRegistry.registryArtifactSha256.slice(0, 16) + '…' : 'Unavailable'} · checked {symbolRegistry.checkedAt}</div>
        <div className="muted" style={{marginTop: 6}}>{symbolRegistry.note}</div>
      </> : <div className="muted">{symbolRegistryError ? 'Multi-exchange symbol registry unavailable. Order routing must remain disabled.' : 'Validating explicit Binance/source → internal → execution-venue identities…'}</div>}
    </Panel>

    <Panel title="Dashboard API Resilience" right={<span className={diagnosticsError ? 'warning' : 'positive'}>{diagnostics ? `v${diagnostics.version}` : diagnosticsError ? 'DIAGNOSTICS UNAVAILABLE' : 'LOADING'}</span>}>
      {diagnostics ? <div className="infra-detail-grid">
        <div><Activity size={14}/><span>Mean REST latency</span><strong>{diagnostics.http.meanLatencyMs.toFixed(1)} ms</strong></div>
        <div><span>HTTP requests</span><strong>{diagnostics.http.requests}</strong></div>
        <div><span>5xx errors</span><strong className={diagnostics.http.serverErrors > 0 ? 'warning' : 'positive'}>{diagnostics.http.serverErrors} · {diagnostics.http.serverErrorRatePct.toFixed(2)}%</strong></div>
        <div><span>Resource timeouts</span><strong className={diagnostics.http.resourceTimeouts > 0 ? 'warning' : ''}>{diagnostics.http.resourceTimeouts}</strong></div>
        <div><span>Active / peak requests</span><strong>{diagnostics.http.activeRequests} / {diagnostics.http.maxActiveRequests}</strong></div>
        <div><span>SSE clients</span><strong>{diagnostics.sse.activeClients} / {diagnostics.sse.maxClients}</strong></div>
        <div><span>SSE rejected</span><strong className={diagnostics.sse.rejected > 0 ? 'warning' : ''}>{diagnostics.sse.rejected}</strong></div>
        <div><span>Goroutines</span><strong>{diagnostics.runtime.goroutines}</strong></div>
        <div><span>Go alloc</span><strong>{formatBytes(diagnostics.runtime.allocBytes)}</strong></div>
        <div><span>Go sys</span><strong>{formatBytes(diagnostics.runtime.sysBytes)}</strong></div>
        <div><Clock3 size={14}/><span>Read timeout</span><strong>{(diagnostics.http.resourceTimeoutMs / 1000).toFixed(1)} s</strong></div>
        <div><span>Uptime</span><strong>{Math.floor(diagnostics.uptimeSeconds / 60)} min</strong></div>
      </div> : <div className="muted">{diagnosticsError ? 'Authenticated diagnostics endpoint is unavailable; trading remains independent from the dashboard.' : 'Loading bounded API runtime metrics…'}</div>}
    </Panel>

    <section className="infra-grid infra-grid--top">
      <Panel title="System Readiness" right={<span className={data.readiness === 'READY' ? 'positive' : 'warning'}>{data.readinessReason}</span>}>
        <div className="dependency-list">{data.dependencies.map(dep => <div className="dependency-row" key={dep.component}><div className="dependency-icon">{dep.state === 'HEALTHY' ? <CheckCircle2 size={14}/> : <TriangleAlert size={14}/>}</div><div><strong>{dep.component}</strong><small>{dep.reason}</small></div><span>{dep.lastCheck}</span><HealthPill state={dep.state}/></div>)}</div>
      </Panel>

      <Panel title="VPS" right={<HealthPill state={data.vps.state}/>}>
        <div className="infra-kv"><div><Server size={14}/><span>Network RX</span><strong>{data.vps.networkRxLabel}</strong></div><div><Network size={14}/><span>Network TX</span><strong>{data.vps.networkTxLabel}</strong></div><div><HardDrive size={14}/><span>Disk used</span><strong>{vpsObserved ? `${data.vps.diskPct.toFixed(0)}%` : '—'}</strong></div><div><Clock3 size={14}/><span>Clock offset</span><strong>{data.vps.clockOffsetLabel}</strong></div></div>
      </Panel>
    </section>

    <Panel title="Containers" right={<span className="muted">Runtime health and resource footprint</span>}>
      <div className="table-wrap"><table className="wide-table infra-table"><thead><tr><th>Container</th><th>State</th><th>Health</th><th>Restarts</th><th>Uptime</th><th>CPU</th><th>RAM</th><th>Last heartbeat</th></tr></thead><tbody>{data.containers.length === 0 && <tr><td colSpan={8} className="muted">Container metrics are not wired in Step 16.</td></tr>}{data.containers.map(row => <tr key={row.name}><td><strong>{row.name}</strong></td><td>{row.state}</td><td><HealthPill state={row.health}/></td><td className={row.restartCount > 0 ? 'warning' : ''}>{row.restartCount}</td><td>{row.uptimeLabel}</td><td>{row.cpuPct.toFixed(1)}%</td><td>{row.ramMb} MB</td><td>{row.lastHeartbeat}</td></tr>)}</tbody></table></div>
    </Panel>

    <section className="infra-grid infra-grid--services">
      <Panel title="Trading Services" right={<span className="muted">Ready state · last event · current mode</span>}>
        <div className="table-wrap"><table><thead><tr><th>Service</th><th>Ready</th><th>Mode</th><th>Last event</th><th>Lag</th><th>Health</th></tr></thead><tbody>{data.services.length === 0 && <tr><td colSpan={6} className="muted">No durable runtime snapshot observed yet; process liveness is not inferred.</td></tr>}{data.services.map(row => <tr key={row.service}><td><strong>{row.service}</strong></td><td className={row.ready ? 'positive' : 'negative'}>{row.ready ? 'YES' : 'NO'}</td><td>{row.mode}</td><td>{row.lastEvent}</td><td>{row.lagLabel}</td><td><HealthPill state={row.health}/></td></tr>)}</tbody></table></div>
      </Panel>

      <div className="infra-stack">
        <Panel title="PostgreSQL" right={<HealthPill state={data.postgres.state}/>}>
          <div className="infra-detail-grid"><div><Database size={14}/><span>Connectivity</span><strong className={data.postgres.connected ? 'positive' : 'negative'}>{data.postgres.connected ? 'CONNECTED' : 'OFFLINE'}</strong></div><div><Activity size={14}/><span>Latency</span><strong>{data.postgres.latencyMs} ms</strong></div><div><span>Connections</span><strong>{data.postgres.activeConnections}/{data.postgres.maxConnections}</strong></div><div><span>Storage</span><strong>{data.postgres.storageUsedLabel}</strong></div><div className="infra-detail-grid__wide"><span>Persistence</span><strong>{data.postgres.persistenceState}</strong></div></div>
        </Panel>

        <Panel title="NATS / JetStream" right={<HealthPill state={data.nats.state}/>}>
          <div className="infra-detail-grid"><div><Radio size={14}/><span>Connection</span><strong className={data.nats.connected ? 'positive' : 'negative'}>{data.nats.connected ? 'CONNECTED' : 'OFFLINE'}</strong></div><div><span>Streams</span><strong>{metric(data.nats.streams)}</strong></div><div><span>Consumers</span><strong>{metric(data.nats.consumers)}</strong></div><div><span>Pending</span><strong>{metric(data.nats.pendingMessages)}</strong></div><div><span>Max lag</span><strong>{metric(data.nats.maxConsumerLag)}</strong></div><div><span>Redeliveries</span><strong>{metric(data.nats.redeliveries)}</strong></div></div>
        </Panel>
      </div>
    </section>

    <section className="infra-grid infra-grid--bottom">
      <Panel title="Exchange Connectivity" right={<HealthPill state={data.exchange.state}/>}>
        <div className="exchange-health"><div className="exchange-health__hero"><Wifi size={22}/><div><strong>{data.exchange.venue}</strong><span>{data.exchange.connected ? 'Connected' : 'Disconnected'} · {data.exchange.reconnectState}</span></div></div><div className="key-value-grid"><span>Last REST/API</span><strong>{data.exchange.lastApiActivity}</strong><span>Last WebSocket</span><strong>{data.exchange.lastWsActivity}</strong><span>API latency</span><strong>{data.exchange.state === 'UNKNOWN' ? '—' : `${data.exchange.apiLatencyMs} ms`}</strong></div></div>
      </Panel>
      <Panel title="Durable Outbox" right={<HealthPill state={data.outbox.state}/>}>
        <div className="outbox-grid"><div><Box size={18}/><span>Pending messages</span><strong>{metric(data.outbox.pendingMessages)}</strong></div><div><Clock3 size={18}/><span>Oldest pending</span><strong>{data.outbox.oldestAgeLabel}</strong></div><div><Activity size={18}/><span>Last published</span><strong>{data.outbox.lastPublished}</strong></div></div>
      </Panel>
    </section>
  </>
}
