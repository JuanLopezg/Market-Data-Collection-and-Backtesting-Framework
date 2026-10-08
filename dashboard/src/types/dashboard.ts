export type HealthState = 'READY' | 'DEGRADED' | 'PAUSED'
export type ReconciliationState = 'CLEAN' | 'PENDING' | 'DRIFT' | 'BLOCKED'
export type PositionAlignmentState = 'ALIGNED' | 'PENDING' | 'DRIFT' | 'BLOCKED' | 'UNKNOWN'
export type Severity = 'INFO' | 'WARN' | 'CRITICAL'
export type TradingMode = 'LIVE' | 'TESTNET' | 'REPLAY' | 'PAPER'
export type PipelineStageState = 'OK' | 'CHANGED' | 'PENDING' | 'BLOCKED'

export interface ShellStatus {
  mode: TradingMode
  exchange: string
  exchangeConnected: boolean
  exchangeState?: string
  exchangeDetail?: string
  dataHealthy: boolean
  dataState?: string
  dataDetail?: string
  tradingEnabled: boolean
  tradingState?: string
  tradingDetail?: string
  reconciliation: ReconciliationState
  readiness: HealthState
  readinessDetail?: string
  alertCount: number
  criticalAlertCount?: number
  warningAlertCount?: number
  utcLabel: string
  blockers?: string[]
  warnings?: string[]
  sourceMode?: 'REAL' | 'MOCK'
}



export type GlobalReadinessState = 'PASS' | 'WARN' | 'DEFERRED' | 'BLOCKED'

export interface GlobalReadinessRequirement {
  id: string
  label: string
  state: GlobalReadinessState
  requiredNow: boolean
  requiredFor: string[]
  evidence: string
  detail: string
  retryable: boolean
}

export interface GlobalReadinessData {
  contractVersion: string
  status: 'VALIDATED_FAIL_CLOSED' | 'BLOCKED' | 'MOCK_ONLY' | string
  contractComplete: boolean
  currentPhase: string
  safeToContinueDashboard: boolean
  privateTestnetReady: boolean
  tradingReady: boolean
  liveReady: boolean
  orderRouting: string
  privateAuth: string
  manualRouting: string
  blockingCount: number
  warningCount: number
  deferredCount: number
  retryableBlockingCount: number
  blockers: string[]
  warnings: string[]
  deferred: string[]
  requirements: GlobalReadinessRequirement[]
  checkedAt: string
  nextSafeStep: string
  futureReplayBoundary: string
  note: string
}

export type SafetyGateState = 'PASS' | 'WARN' | 'BLOCKED'

export interface SafetyGateCheck {
  id: string
  label: string
  state: SafetyGateState
  detail: string
}

export interface SafetyGateData {
  status: SafetyGateState
  safeToProceed: boolean
  tradingReady: boolean
  runtimeReadiness: string
  endToEndProofStatus: string
  endToEndCompletion: string
  checkedAt: string
  checks: SafetyGateCheck[]
  note: string
}


export interface VenueFoundationData {
  status: string
  foundationReady: boolean
  venue: string
  targetEnvironment: string
  gatewayMode: string
  publicConnectivity: string
  privateAuth: string
  orderRouting: string
  symbolMapping: string
  exchangeFilters: string
  secretsRequired: boolean
  capitalRequired: boolean
  readOnly: boolean
  sourceContract: string
  note: string
}


export interface VenuePublicData {
  status: string
  connected: boolean
  venue: string
  targetEnvironment: string
  endpoint: string
  metadataStatus: string
  midsStatus: string
  universeCount: number
  midCount: number
  matchedMidCount: number
  sampleSymbols: string[]
  latencyMs: number
  checkedAt: string
  privateAuth: string
  orderRouting: string
  symbolMapping: string
  exchangeFilters: string
  secretsUsed: boolean
  capitalUsed: boolean
  readOnly: boolean
  error?: string
  note: string
}

export interface VenueSymbolMappingRow {
  internal: string
  venue: string
  state: string
  reason?: string
}

export interface VenueSymbolMappingData {
  status: string
  validated: boolean
  executionCoverageComplete: boolean
  venue: string
  targetEnvironment: string
  policy: string
  mappingArtifactSha256: string
  mappingEntryCount: number
  unsupportedCatalogEntryCount: number
  classificationEntryCount: number
  requiredSymbolCount: number
  classifiedRequiredCount: number
  mappedRequiredCount: number
  unsupportedRequiredCount: number
  missingInternalSymbols: string[]
  missingVenueSymbols: string[]
  unsupportedInternalSymbols: string[]
  rows: VenueSymbolMappingRow[]
  metadataLatencyMs: number
  checkedAt: string
  privateAuth: string
  orderRouting: string
  exchangeFilters: string
  readOnly: boolean
  error?: string
  note: string
}

export interface VenueTradingRuleRow {
  internal: string
  venue: string
  state: string
  sizeDecimals: number
  sizeStepLabel: string
  priceMaxSignificantFigures: number
  priceMaxDecimals: number
  maxLeverage: number
  onlyIsolated: boolean
  marginMode?: string
  isDelisted: boolean
  midPrice: string
  minOrderNotionalUsd: number
  estimatedMinOrderSizeLabel: string
  estimatedMinOrderNotionalUsd: number
  reason?: string
}

export interface VenueTradingRulesData {
  status: string
  validated: boolean
  venue: string
  targetEnvironment: string
  supportedMappingCount: number
  nonRoutableMappingCount: number
  validatedRuleCount: number
  blockedRuleCount: number
  priceMaxSignificantFigures: number
  perpMaxDecimals: number
  integerPricesAlwaysAllowed: boolean
  minOrderNotionalUsd: number
  rows: VenueTradingRuleRow[]
  metadataLatencyMs: number
  checkedAt: string
  privateAuth: string
  orderRouting: string
  readOnly: boolean
  ruleSource: string
  error?: string
  note: string
}


export interface SymbolRegistryAlarm {
  severity: Severity
  eventType: string
  asset?: string
  title: string
  detail: string
}

export interface SymbolRegistryRow {
  internal: string
  marketDataVenue: string
  marketDataSymbol: string
  executionVenue: string
  executionEnvironment: string
  executionSymbol: string
  registryState: string
  runtimeState: string
  routingPolicy: string
  reason?: string
  inCurrentRanking: boolean
  inCurrentStrategy: boolean
  venuePresent: boolean
}

export interface SymbolRegistryData {
  status: string
  validated: boolean
  registryVersion: string
  registryArtifactSha256: string
  policy: string
  marketDataSource: string
  executionVenue: string
  executionEnvironment: string
  registryEntryCount: number
  currentRankingCount: number
  currentRankingRegisteredCount: number
  currentStrategyCount: number
  currentStrategyRegisteredCount: number
  currentStrategyRoutableCount: number
  currentStrategyNonRoutableCount: number
  executionCoverageComplete: boolean
  unregisteredRankingSymbols: string[]
  unregisteredStrategySymbols: string[]
  venueAbsentStrategySymbols: string[]
  explicitBlockedStrategySymbols: string[]
  mappingDivergences: string[]
  alarms: SymbolRegistryAlarm[]
  rows: SymbolRegistryRow[]
  metadataLatencyMs: number
  checkedAt: string
  privateAuth: string
  orderRouting: string
  readOnly: boolean
  error?: string
  note: string
}

export interface LedgerEntry {
  entryId: string
  fillId: string
  orderId: string
  strategyId: number
  timestamp: string
  asset: string
  side: 'BUY' | 'SELL'
  quantity: number
  price: number
  grossNotional: number
  commission: number
  positionDelta: number
  cashDelta: number
  entryHash: string
}

export interface LedgerData {
  status: string
  validated: boolean
  foundationReady: boolean
  sourceTable: string
  sourceContract: string
  appendOnlySource: boolean
  deterministicProjection: boolean
  totalFillRows: number
  distinctFillIds: number
  invalidFillRows: number
  latestFillId: number
  latestFillTimestamp: number
  totalFees: number
  totalFeesLabel: string
  grossBuyNotional: number
  grossSellNotional: number
  netCashDeltaFromFills: number
  recentWindowCount: number
  recentWindowTruncated: boolean
  recentWindowFingerprint: string
  entries: LedgerEntry[]
  durableRealizedPnl: boolean
  durableUnrealizedPnl: boolean
  historicalEquityAvailable: boolean
  readOnly: boolean
  privateAuth: string
  orderRouting: string
  checkedAt: string
  error?: string
  note: string
}

export interface ProviderStatus {
  name: string
  mode: 'mock' | 'real'
  ready: boolean
  detail: string
  resourceCount: number
}

export interface StatMetric {
  label: string
  value: string
  delta?: string
  detail?: string
  trend?: number[]
}

export interface EquityPoint {
  label: string
  equity: number
  benchmark: number
}

export interface Position {
  asset: string
  side: 'Long' | 'Short'
  quantity: number
  quantityLabel: string
  entryPrice: number
  entryPriceLabel: string
  currentPrice: number
  currentPriceLabel: string
  pnlUsd: number
  pnlUsdLabel: string
  pnlPct: number
  pnlPctLabel: string
  currentWeightPct: number
  targetWeightPct: number
  targetQty: number
  effectiveQty: number
  localQty: number
  exchangeQty: number
  deltaQty: number
  status: PositionAlignmentState
  valuationAvailable?: boolean
  targetAvailable?: boolean
  pnlAvailable?: boolean
  reconciliationAvailable?: boolean
  strategyBreakdown?: Array<{ strategyId: number; quantity: number; quantityLabel: string }>
}

export interface RecentEvent {
  time: string
  event: string
  source: string
  severity: Severity
}

export interface OverviewCheck {
  label: string
  state: PositionAlignmentState
  detail: string
}

export interface OverviewData {
  stats: StatMetric[]
  equityCurve: EquityPoint[]
  liveExpected: EquityPoint[]
  positions: Position[]
  recentEvents: RecentEvent[]
  checks?: OverviewCheck[]
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  sourceNote?: string
  readiness?: HealthState
  readinessDetail?: string
  reconciliationStatus?: ReconciliationState
  equityAvailable?: boolean
  liveExpectedAvailable?: boolean
  marketDataStatus?: PositionAlignmentState
  canonicalCycle?: string
  sourceWarnings?: string[]
}

export interface PositionsData {
  totalEquity: string
  activePositions: number
  grossExposure: string
  grossExposurePct: string
  netExposure: string
  unrealizedPnl: string
  positions: Position[]
  accountCash?: string
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  valuationState?: 'AVAILABLE' | 'UNAVAILABLE'
  sourceNote?: string
}

export interface ReconciliationRow {
  asset: string
  targetQty: number
  targetQtyLabel?: string
  targetAvailable?: boolean
  effectiveQty: number
  effectiveQtyLabel?: string
  localQty: number
  localQtyLabel?: string
  exchangeQty: number
  exchangeQtyLabel?: string
  exchangeAvailable?: boolean
  deltaQty: number
  deltaQtyLabel?: string
  pendingQty: number
  pendingQtyLabel?: string
  status: PositionAlignmentState
  explanation: string
}

export interface OpenOrderSummary {
  orderId: string
  asset: string
  side: 'BUY' | 'SELL'
  quantity: number
  remaining: number
  age: string
  state: 'NEW' | 'PARTIAL' | 'PENDING_CANCEL'
  exchangeId: string
}

export interface ReconciliationIssue {
  kind: string
  asset: string
  orderId: string
  localValue: number
  exchangeValue: number
  message: string
}

export interface ReconciliationData {
  status: ReconciliationState
  lastChecked: string
  tolerance: string
  targetPortfolioValue: string
  localPortfolioValue: string
  exchangePortfolioValue: string
  rows: ReconciliationRow[]
  openOrders: OpenOrderSummary[]
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  sourceNote?: string
  comparisonAvailable?: boolean
  exchangeEvidenceTime?: string
  exchangeEvidenceSequence?: number
  exchangeSnapshotTimestamp?: number
  evidenceFresh?: boolean
  issues?: ReconciliationIssue[]
}

export interface PipelineStage {
  id: 'market' | 'indicators' | 'signal' | 'raw-target' | 'vol-target' | 'risk' | 'approved-target' | 'current-position' | 'delta' | 'order-plan' | 'exchange'
  label: string
  value: string
  detail: string
  state: PipelineStageState
}

export interface PipelineAssetRow {
  cycleId: string
  asset: string
  decisionTime: string
  rank: number
  rankLabel?: string
  rsi: number
  rsiLabel?: string
  signal: 'LONG' | 'SHORT' | 'FLAT'
  rawTargetPct: number
  rawTargetLabel?: string
  volTargetPct: number
  volTargetLabel?: string
  approvedTargetPct: number
  approvedTargetLabel?: string
  currentWeightPct: number
  currentWeightLabel?: string
  requiredDeltaPct: number
  requiredDeltaLabel?: string
  plannedAction: string
  exchangeState: 'FILLED' | 'PARTIAL' | 'PENDING' | 'NONE' | 'REJECTED' | 'CANCELED'
  stages: PipelineStage[]
}

export interface WhyTraceMarket {
  price: string
  universe: string
  liquidityRank: string
  freshness: string
}

export interface WhyTraceStrategy {
  rsi: string
  signal: string
  transition: string
  intent: string
}

export interface WhyTracePortfolio {
  rawTarget: string
  volTarget: string
  riskChanges: string[]
  approvedTarget: string
}

export interface WhyTracePlanning {
  effectiveQty: string
  pendingQty: string
  delta: string
  stateRevision: string
}

export interface WhyTraceExecution {
  orderIds: string[]
  side: string
  quantity: string
  fillState: string
  fees: string
  slippage: string
}

export interface WhyTraceReconciliation {
  localQty: string
  exchangeQty: string
  status: PositionAlignmentState
}

export interface WhyTraceAudit {
  correlationId: string
  configVersion: string
  mode: TradingMode
  exchange: string
  decisionTimestamp: string
  executionTimestamp: string
}

export interface WhyTrace {
  cycleId: string
  asset: string
  market: WhyTraceMarket
  strategy: WhyTraceStrategy
  portfolio: WhyTracePortfolio
  planning: WhyTracePlanning
  execution: WhyTraceExecution
  reconciliation: WhyTraceReconciliation
  audit: WhyTraceAudit
}


export interface EndToEndProofStep {
  id: string
  label: string
  state: PositionAlignmentState
  evidence: string
  source: string
}

export interface EndToEndProof {
  status: PositionAlignmentState
  completion: 'FULL_CHAIN' | 'NO_ACTION' | 'INCOMPLETE_OR_IN_FLIGHT' | 'CONTRADICTION_OR_TERMINAL_FAILURE' | string
  cycleTimestamp: string
  correlationId: string
  submitOrders: number
  cancelOrders: number
  matchedOrders: number
  matchedFills: number
  orderIds: string[]
  fillIds: string[]
  steps: EndToEndProofStep[]
  sourceNote: string
}

export interface PipelineData {
  proof?: EndToEndProof
  cycleLabel: string
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  sourceNote?: string
  latestDecision: string
  universeSize: number
  activeSignals: number
  actionablePlans: number
  rows: PipelineAssetRow[]
  traces: Record<string, WhyTrace>
}

export type ExecutionOrderState = 'NEW' | 'PARTIAL' | 'FILLED' | 'REJECTED' | 'CANCELED' | 'PENDING_CANCEL'
export type LifecycleTone = 'good' | 'warn' | 'bad' | 'info' | 'muted'

export interface ExecutionLifecycleEvent {
  time: string
  state: string
  detail: string
  tone: LifecycleTone
}

export interface ExecutionOrder {
  orderId: string
  strategyId?: number
  exchangeOrderId: string
  cycleId: string
  correlationId: string
  asset: string
  side: 'BUY' | 'SELL'
  quantity: number
  filledQty: number
  remainingQty: number
  state: ExecutionOrderState
  age: string
  expectedPriceLabel: string
  avgFillPriceLabel: string
  feesLabel: string
  slippageBps: number
  slippageBpsLabel: string
  submitLatencyMs: number
  fillLatencyMs: number | null
  submittedAt: string
  lastUpdateAt: string
  lifecycle: ExecutionLifecycleEvent[]
}

export interface PartialFill {
  fillId: string
  orderId: string
  asset: string
  quantity: number
  priceLabel: string
  cumulativeLabel: string
  feesLabel: string
  timestamp: string
}

export interface ExecutionReject {
  orderId: string
  asset: string
  code: string
  reason: string
  remediation: string
}

export interface CancelReplacement {
  asset: string
  originalOrderId: string
  cancelRequestedAt: string
  canceledAt: string
  replacementOrderId: string
  replacementState: ExecutionOrderState
}

export interface ExecutionData {
  lastUpdated: string
  openOrders: number
  partialOrders: number
  pendingCancels: number
  filledOrders: number
  fillCount: number
  rejectCount: number
  avgSubmitLatencyMs: number
  avgFillLatencyMs: number
  avgSlippageBpsLabel: string
  submitLatencyP95Ms: number
  fillLatencyP95Ms: number
  bestSlippageBpsLabel: string
  worstSlippageBpsLabel: string
  totalFeesLabel: string
  rejectRateLabel: string
  orders: ExecutionOrder[]
  partialFills: PartialFill[]
  rejects: ExecutionReject[]
  replacements: CancelReplacement[]
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  sourceNote?: string
  latencyAvailable?: boolean
  slippageAvailable?: boolean
  orderWindowTruncated?: boolean
  fillWindowTruncated?: boolean
}

export type RiskLimitState = 'OK' | 'WARN' | 'BREACH'

export interface RiskLimitMetric {
  id: string
  label: string
  currentLabel: string
  limitLabel: string
  utilizationPct: number
  state: RiskLimitState
}

export interface AssetRiskLimit {
  asset: string
  currentWeightLabel: string
  approvedWeightLabel: string
  maxWeightLabel: string
  utilizationPct: number
  state: RiskLimitState
}

export interface RiskBreach {
  rule: string
  currentLabel: string
  thresholdLabel: string
  state: RiskLimitState
  action: string
}

export interface RiskSafetyState {
  tradingState: 'TRADING READY' | 'TRADING PAUSED'
  reason: string
  killSwitch: 'ARMED' | 'TRIGGERED'
  lastRiskCheck: string
  riskRevision: string
}

export interface ExchangeAllocation {
  assignedLabel: string
  usedLabel: string
  freeLabel: string
  lockedLabel: string
  lockedPct: number
}

export interface RealRiskAsset {
  asset: string
  signal: 'LONG' | 'SHORT' | 'FLAT'
  approvedWeightLabel: string
  targetNotionalLabel: string
  strategyLabel: string
  currentWeightLabel: string
  limitLabel: string
}

export interface RiskData {
  riskState: string
  portfolioLimits: RiskLimitMetric[]
  assetLimits: AssetRiskLimit[]
  breaches: RiskBreach[]
  safety: RiskSafetyState
  exchangeAllocation: ExchangeAllocation
  sourceMode?: 'MOCK' | 'REAL'
  sourceUpdatedAt?: string
  sourceNote?: string
  decisionTimestamp?: string
  decisionCorrelationId?: string
  decisionMessageId?: string
  accountMessageId?: string
  signalsMessageId?: string
  referenceCapitalLabel?: string
  accountCashLabel?: string
  approvedGrossTargetLabel?: string
  approvedNetTargetLabel?: string
  approvedTargetNotionalLabel?: string
  activeTargetCount?: number
  nonZeroSignalCount?: number
  strategyCount?: number
  realAssets?: RealRiskAsset[]
  missingDiagnostics?: string[]
  activeLimitsAvailable?: boolean
  breachesAvailable?: boolean
  currentValuationAvailable?: boolean
}

export type MarketDataState = 'HEALTHY' | 'STALE' | 'WARN' | 'INVALID' | 'VALID'

export interface MarketFreshnessRow {
  asset: string
  lastCandle: string
  ageLabel: string
  staleThresholdLabel: string
  source: string
  state: MarketDataState
}

export interface MarketIntegrity {
  missingCandles: number
  duplicateTimestamps: number
  gaps: number
  invalidRows: number
}

export interface UniverseDiagnosticRow {
  rank: number
  asset: string
  smaVolumeLabel: string
  rsi: number
  signal: 'LONG' | 'SHORT' | 'FLAT'
  signalState: string
  slotState: string
  diagnostic: string
  diagnosticTone: 'normal' | 'warn' | 'bad'
}

export interface CandidateRejection {
  asset: string
  reason: string
  detail: string
  severity: MarketDataState
}

export interface StrategySnapshot {
  ranking: string
  entryRule: string
  exitRule: string
  maxPositions: string
  semantics: string
}

export interface MarketDataData {
  liquidityLabel?: string
  source: string
  latestCompletedCandle: string
  healthyAssets: number
  staleAssets: number
  totalAssets: number
  universeSize: number
  activeSignals: number
  availableSlots: number
  freshness: MarketFreshnessRow[]
  integrity: MarketIntegrity
  universe: UniverseDiagnosticRow[]
  candidateRejections: CandidateRejection[]
  strategySnapshot: StrategySnapshot
  sourceMode?: 'REAL' | 'MOCK'
  sourceUpdatedAt?: string
  sourceNote?: string
  signalDataAvailable?: boolean
  signalCycleAligned?: boolean
  canonicalTopN?: number
  historyDays?: number
}

export type InfrastructureHealth = 'HEALTHY' | 'WARN' | 'CRITICAL' | 'UNKNOWN'
export type ContainerState = 'RUNNING' | 'RESTARTING' | 'STOPPED'
export type AlertStatus = 'ACTIVE' | 'ACKNOWLEDGED' | 'RESOLVED'
export type AuditActorType = 'SYSTEM' | 'OPERATOR' | 'HUMAN'

export interface VpsHealth {
  ramLabel?: string
  diskLabel?: string
  diskAvailableGiB?: number
  clockObserved?: boolean
  cpuPct: number
  ramPct: number
  diskPct: number
  load1m: number
  networkRxLabel: string
  networkTxLabel: string
  uptimeLabel: string
  clockOffsetLabel: string
  clockSynced: boolean
  state: InfrastructureHealth
}

export interface ContainerHealthRow {
  name: string
  state: ContainerState
  health: InfrastructureHealth
  restartCount: number
  uptimeLabel: string
  cpuPct: number
  ramMb: number
  lastHeartbeat: string
}

export interface TradingServiceHealthRow {
  processRunning?: boolean
  service: string
  ready: boolean
  mode: TradingMode
  lastEvent: string
  lagLabel: string
  health: InfrastructureHealth
}

export interface PostgresHealth {
  connected: boolean
  latencyMs: number
  activeConnections: number
  maxConnections: number
  storageUsedLabel: string
  storagePct: number
  persistenceState: string
  state: InfrastructureHealth
}

export interface NatsHealth {
  connected: boolean
  streams: number
  consumers: number
  pendingMessages: number
  maxConsumerLag: number
  redeliveries: number
  ackHealthLabel: string
  state: InfrastructureHealth
}

export interface ExchangeHealth {
  venue: string
  connected: boolean
  reconnectState: string
  lastApiActivity: string
  lastWsActivity: string
  apiLatencyMs: number
  state: InfrastructureHealth
}

export interface OutboxHealth {
  pendingMessages: number
  oldestAgeLabel: string
  lastPublished: string
  state: InfrastructureHealth
}

export interface ReadinessDependency {
  component: string
  state: InfrastructureHealth
  reason: string
  lastCheck: string
}

export interface InfrastructureData {
  telemetryObservedAt?: string
  readiness: HealthState
  readinessReason: string
  lastUpdated: string
  vps: VpsHealth
  containers: ContainerHealthRow[]
  services: TradingServiceHealthRow[]
  postgres: PostgresHealth
  nats: NatsHealth
  exchange: ExchangeHealth
  outbox: OutboxHealth
  dependencies: ReadinessDependency[]
  sourceMode?: string
  sourceNote?: string
}

export interface OperationalAlert {
  id: string
  timestamp: string
  severity: Severity
  status: AlertStatus
  service: string
  asset?: string
  eventType: string
  title: string
  detail: string
  correlationId?: string
  linkedContext?: string
  acknowledgementKey?: string
  acknowledgedAt?: string
  acknowledgedBy?: string
  acknowledgementComment?: string
}

export interface AuditRecord {
  id: string
  timestamp: string
  actor: string
  actorType: AuditActorType
  action: string
  target: string
  result: 'SUCCESS' | 'REJECTED' | 'NOOP'
  requestHash?: string
  correlationId?: string
  detail: string
}

export interface DurableAlertLifecycleEvent {
  eventId: string
  recordedAt: string
  transition: 'OPENED' | 'UPDATED' | 'RESOLVED' | string
  alert: OperationalAlert
}

export interface AlertsAuditData {
  activeCritical: number
  activeWarnings: number
  acknowledged: number
  resolved24h: number
  alerts: OperationalAlert[]
  audit: AuditRecord[]
  sourceMode?: 'REAL' | 'MOCK'
  sourceUpdatedAt?: string
  sourceNote?: string
  acknowledgementAvailable?: boolean
  durableAlertHistoryAvailable?: boolean
  humanAuditAvailable?: boolean
  auditMode?: 'DERIVED_SYSTEM_EVIDENCE' | 'DURABLE_AUDIT' | string
  watchdogAvailable?: boolean
  watchdogState?: 'HEALTHY' | 'DEGRADED' | 'STALE' | 'UNAVAILABLE' | 'NOT_WIRED' | string
  watchdogLastSweepAt?: string
  watchdogLastSuccessAt?: string
  durableEventCount?: number
  durableLifecycleEvents?: DurableAlertLifecycleEvent[]
  acknowledgementEventCount?: number
}

export interface AlertAcknowledgementInput {
  alertId: string
  acknowledgementKey: string
  comment?: string
}

export interface AlertAcknowledgementResult {
  status: 'ACKNOWLEDGED'
  persisted: boolean
  alreadyAcknowledged: boolean
  alertId: string
  acknowledgementKey: string
  actor: string
  recordedAt: string
  correlationId: string
  comment?: string
  resolutionState: 'UNRESOLVED'
  tradingStateMutated: false
  note: string
}

export type BehaviourClassification = 'NORMAL' | 'ELEVATED' | 'ABNORMAL' | 'CRITICAL'
export type BehaviourMetricFamily = 'ACTIVITY' | 'HOLDING' | 'RISK' | 'PERFORMANCE' | 'EXECUTION' | 'STRATEGY INPUTS'

export interface BehaviourTrendPoint {
  label: string
  live: number
  mean: number
  lower: number
  upper: number
}

export interface BehaviourCycleContribution {
  timestamp: string
  asset: string
  cycleId: string
  valueLabel: string
  deviationLabel: string
}

export interface BehaviourMetric {
  id: string
  family: BehaviourMetricFamily
  label: string
  currentLabel: string
  historicalMeanLabel: string
  historicalRangeLabel: string
  zScore: number
  classification: BehaviourClassification
  description: string
  trend: BehaviourTrendPoint[]
  distribution: number[]
  contributingCycles: BehaviourCycleContribution[]
}

export type LiveVsExpectedCoverageState = 'VALIDATED' | 'DEFERRED' | 'BLOCKED' | string

export interface LiveVsExpectedCoverage {
  id: string
  label: string
  state: LiveVsExpectedCoverageState
  source: string
  detail: string
}

export interface LiveVsExpectedAnomaly {
  metricId: string
  label: string
  classification: BehaviourClassification
  zScore: number
  currentLabel: string
  detail: string
}

export interface LiveVsExpectedData {
  contractVersion: string
  status: string
  validated: boolean
  projectionReady: boolean
  observationMode: string
  baselineLabel: string
  baselineWindow: string
  baselineFingerprint: string
  baselineObservationCount: number
  baselineExcludesLatest: boolean
  latestObservation: string
  metricCount: number
  normalCount: number
  elevatedCount: number
  abnormalCount: number
  criticalCount: number
  overallClassification: BehaviourClassification | 'INSUFFICIENT_DATA' | string
  anomalyCount: number
  anomalies: LiveVsExpectedAnomaly[]
  coverage: LiveVsExpectedCoverage[]
  metrics: BehaviourMetric[]
  sourceMode?: 'REAL' | 'MOCK'
  sourceNote?: string
  readOnly: boolean
  privateAuth: string
  orderRouting: string
  checkedAt: string
  error?: string
}

export type ManualControlStage = 'UPLOAD' | 'VALIDATE' | 'PREVIEW' | 'RISK_CHECK' | 'ORDER_PREVIEW' | 'CONFIRM'
export type ManualValidationSeverity = 'ERROR' | 'WARN' | 'INFO'
export type ManualOrderAction = 'BUY' | 'SELL' | 'HOLD'

export interface ManualPortfolioTemplateRow {
  asset: string
  currentWeightPct: number
  requestedWeightPct: number
  approvedWeightPct: number
  estimatedNotionalLabel: string
  deltaPct: number
  riskNote?: string
}

export interface ManualValidationIssue {
  severity: ManualValidationSeverity
  field: string
  message: string
}

export interface ManualOrderPreviewRow {
  asset: string
  action: ManualOrderAction
  deltaWeightPct: number
  estimatedNotionalLabel: string
  estimatedNotionalUsd?: number
  estimatedFeeLabel: string
  note: string
}

export interface ManualRouteAuditRow {
  recordedAt: string
  actor: string
  result: string
  requestHash: string
  correlationId: string
  referenceTargetTimestamp: string
  submitted: boolean
  blockers: string[]
  detail: string
}

export interface ManualControlData {
  contractVersion?: string
  routingContractReady?: boolean
  routingContractMode?: string
  executionBoundary?: string
  exchangeConstraintsValidated?: boolean
  registryVersion?: string
  registryArtifactSha256?: string
  venueRulesCheckedAt?: string
  tradingControlSink?: string
  privateAuth?: string
  orderLifecycle?: string
  confirmationRequired?: boolean
  confirmationPhrase?: string
  humanAuditAvailable?: boolean
  routeBlockers?: string[]
  recentRouteAudits?: ManualRouteAuditRow[]
  mode: TradingMode
  exchange: string
  schemaLabel: string
  maxUploadSizeLabel: string
  exampleCsv: string
  requestHash: string
  previewRows: ManualPortfolioTemplateRow[]
  validationIssues: ManualValidationIssue[]
  orderPreview: ManualOrderPreviewRow[]
  estimatedFeesLabel: string
  estimatedTurnoverLabel: string
  auditActorLabel: string
  backendAuthoritative?: boolean
  validationPassed?: boolean
  riskCheckAvailable?: boolean
  routeEnabled?: boolean
  sourceMode?: 'MOCK' | 'REAL'
  currentTargetTimestamp?: string
  previewKind?: string
  safetyNote?: string
}

export interface ManualControlPreviewInput {
  filename: string
  csv: string
}

export interface ManualControlRouteInput extends ManualControlPreviewInput {
  requestHash: string
  referenceTargetTimestamp: string
  confirmation: string
}

export interface ManualControlRouteResult {
  contractVersion: string
  status: 'BLOCKED' | 'ACCEPTED' | string
  submitted: boolean
  routeEnabled: boolean
  confirmationAccepted: boolean
  requestHash: string
  correlationId: string
  actor: string
  referenceTargetTimestamp: string
  checkedAt: string
  blockers: string[]
  auditPersisted: boolean
  note: string
}

export interface DashboardDiagnostics {
  version: string
  startedAt: string
  uptimeSeconds: number
  provider: {
    name: string
    mode: string
    ready: boolean
  }
  http: {
    requests: number
    activeRequests: number
    maxActiveRequests: number
    serverErrors: number
    serverErrorRatePct: number
    meanLatencyMs: number
    resourceTimeouts: number
    resourceTimeoutMs: number
  }
  sse: {
    activeClients: number
    maxClients: number
    rejected: number
  }
  runtime: {
    goroutines: number
    allocBytes: number
    sysBytes: number
    heapObjects: number
  }
}
