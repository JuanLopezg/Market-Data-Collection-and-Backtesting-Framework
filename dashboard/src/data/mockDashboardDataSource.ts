import type { DashboardDataSource } from './dashboardDataSource'
import { runtimeConfig } from '../config/runtime'
import type { AlertsAuditData, DashboardDiagnostics, ExecutionData, InfrastructureData, LiveVsExpectedData, ManualControlData, MarketDataData, OverviewData, PipelineData, Position, PositionsData, ReconciliationData, RiskData, ShellStatus, ProviderStatus, SafetyGateData, GlobalReadinessData, VenueFoundationData, VenuePublicData, VenueSymbolMappingData, VenueTradingRulesData, SymbolRegistryData, LedgerData } from '../types/dashboard'

const wait = (ms = runtimeConfig.mockLatencyMs) => new Promise(resolve => setTimeout(resolve, ms))

const equity = [690, 735, 760, 815, 845, 875, 905, 940, 975, 1010, 1035, 1075, 1105, 1140, 1175, 1200, 1240, 1280, 1315, 1360, 1410]
const benchmark = [650, 675, 690, 710, 735, 750, 770, 795, 805, 825, 835, 850, 875, 895, 920, 940, 960, 990, 1015, 1050, 1090]
const labels = ['Sep 03','Sep 04','Sep 05','Sep 06','Sep 07','Sep 08','Sep 09','Sep 10','Sep 11','Sep 12','Sep 13','Sep 14','Sep 15','Sep 16','Sep 17','Sep 18','Sep 19','Sep 20','Sep 21','Sep 22','Sep 23']


const diagnosticsData: DashboardDiagnostics = {
  version: '0.46.0-mock',
  startedAt: '2026-09-25T08:00:00Z',
  uptimeSeconds: 3600,
  provider: { name: 'frontend-local-mocks', mode: 'mock', ready: true },
  http: { requests: 128, activeRequests: 1, maxActiveRequests: 4, serverErrors: 0, serverErrorRatePct: 0, meanLatencyMs: 8.4, resourceTimeouts: 0, resourceTimeoutMs: 7000 },
  sse: { activeClients: 1, maxClients: 16, rejected: 0 },
  runtime: { goroutines: 12, allocBytes: 12_582_912, sysBytes: 29_360_128, heapObjects: 14_200 },
}

const providerStatus: ProviderStatus = { name: 'frontend-local-mocks', mode: 'mock', ready: true, detail: 'Direct Vite mock data', resourceCount: 12 }

const venueFoundationData: VenueFoundationData = {
  status: 'MOCK_ONLY',
  foundationReady: false,
  venue: 'MOCK',
  targetEnvironment: 'MOCK',
  gatewayMode: 'mock',
  publicConnectivity: 'NOT_CHECKED',
  privateAuth: 'DISABLED',
  orderRouting: 'DISABLED',
  symbolMapping: 'NOT_CONFIGURED',
  exchangeFilters: 'NOT_LOADED',
  secretsRequired: false,
  capitalRequired: false,
  readOnly: true,
  sourceContract: 'mock provider',
  note: 'Mock mode cannot satisfy the Step 33 testnet foundation gate.',
}

const venuePublicData: VenuePublicData = {
  status: 'MOCK_ONLY',
  connected: false,
  venue: 'MOCK',
  targetEnvironment: 'MOCK',
  endpoint: '',
  metadataStatus: 'NOT_CHECKED',
  midsStatus: 'NOT_CHECKED',
  universeCount: 0,
  midCount: 0,
  matchedMidCount: 0,
  sampleSymbols: [],
  latencyMs: 0,
  checkedAt: 'mock',
  privateAuth: 'DISABLED',
  orderRouting: 'DISABLED',
  symbolMapping: 'NOT_CONFIGURED',
  exchangeFilters: 'NOT_APPLIED',
  secretsUsed: false,
  capitalUsed: false,
  readOnly: true,
  note: 'Mock mode does not contact Hyperliquid and cannot satisfy the Step 34 public venue connectivity gate.',
}

const venueSymbolMappingData: VenueSymbolMappingData = {
  status: 'MOCK_ONLY', validated: false, executionCoverageComplete: false, venue: 'MOCK', targetEnvironment: 'MOCK', policy: 'EXPLICIT_ONLY',
  mappingArtifactSha256: '', mappingEntryCount: 0, unsupportedCatalogEntryCount: 0, classificationEntryCount: 0,
  requiredSymbolCount: 0, classifiedRequiredCount: 0, mappedRequiredCount: 0, unsupportedRequiredCount: 0,
  missingInternalSymbols: [], missingVenueSymbols: [], unsupportedInternalSymbols: [], rows: [], metadataLatencyMs: 0, checkedAt: 'mock',
  privateAuth: 'DISABLED', orderRouting: 'DISABLED', exchangeFilters: 'NOT_APPLIED', readOnly: true,
  note: 'Mock mode cannot satisfy the Step 35 explicit symbol classification gate.',
}

const venueTradingRulesData: VenueTradingRulesData = {
  status: 'MOCK_ONLY', validated: false, venue: 'MOCK', targetEnvironment: 'MOCK',
  supportedMappingCount: 0, nonRoutableMappingCount: 0, validatedRuleCount: 0, blockedRuleCount: 0,
  priceMaxSignificantFigures: 5, perpMaxDecimals: 6, integerPricesAlwaysAllowed: true, minOrderNotionalUsd: 10, rows: [], metadataLatencyMs: 0, checkedAt: 'mock',
  privateAuth: 'DISABLED', orderRouting: 'DISABLED', readOnly: true, ruleSource: 'mock provider',
  note: 'Mock mode cannot satisfy the Step 36 public venue trading-rules gate.',
}


const symbolRegistryData: SymbolRegistryData = {
  status: 'MOCK_ONLY', validated: false, registryVersion: 'mock', registryArtifactSha256: '', policy: 'EXPLICIT_ONLY',
  marketDataSource: 'BINANCE', executionVenue: 'HYPERLIQUID', executionEnvironment: 'TESTNET', registryEntryCount: 0,
  currentRankingCount: 0, currentRankingRegisteredCount: 0, currentStrategyCount: 0, currentStrategyRegisteredCount: 0,
  currentStrategyRoutableCount: 0, currentStrategyNonRoutableCount: 0, executionCoverageComplete: false,
  unregisteredRankingSymbols: [], unregisteredStrategySymbols: [], venueAbsentStrategySymbols: [], explicitBlockedStrategySymbols: [],
  mappingDivergences: [], alarms: [], rows: [], metadataLatencyMs: 0, checkedAt: 'mock', privateAuth: 'DEFERRED', orderRouting: 'DISABLED',
  readOnly: true, note: 'Mock mode cannot validate the real multi-exchange symbol registry.',
}

const ledgerData: LedgerData = {
  status: 'MOCK_ONLY', validated: false, foundationReady: false, sourceTable: 'mock', sourceContract: 'mock provider',
  appendOnlySource: false, deterministicProjection: false, totalFillRows: 0, distinctFillIds: 0, invalidFillRows: 0, latestFillId: 0, latestFillTimestamp: 0,
  totalFees: 0, totalFeesLabel: '$0.00', grossBuyNotional: 0, grossSellNotional: 0, netCashDeltaFromFills: 0,
  recentWindowCount: 0, recentWindowTruncated: false, recentWindowFingerprint: '0'.repeat(64), entries: [],
  durableRealizedPnl: false, durableUnrealizedPnl: false, historicalEquityAvailable: false, readOnly: true, privateAuth: 'DEFERRED', orderRouting: 'DISABLED', checkedAt: 'mock',
  note: 'Mock mode cannot validate the real Step 42 append-only ledger foundation.',
}

const safetyGateData: SafetyGateData = {
  status: 'BLOCKED',
  safeToProceed: false,
  tradingReady: false,
  runtimeReadiness: 'MOCK',
  endToEndProofStatus: 'MOCK',
  endToEndCompletion: 'NOT_APPLICABLE',
  checkedAt: 'mock',
  checks: [
    { id: 'provider', label: 'Real data provider', state: 'BLOCKED', detail: 'Frontend mock mode cannot satisfy the Step 32 pre-testnet safety gate.' },
    { id: 'command-surface', label: 'Dashboard command surface', state: 'PASS', detail: 'Mock UI exposes no real trading route.' },
  ],
  note: 'Switch to the authenticated API with DASHBOARD_DATA_PROVIDER=real for the Step 32 gate.',
}

const globalReadinessData: GlobalReadinessData = {
  contractVersion: 'step44-v1',
  status: 'MOCK_ONLY',
  contractComplete: true,
  currentPhase: 'FRONTEND_MOCK',
  safeToContinueDashboard: false,
  privateTestnetReady: false,
  tradingReady: false,
  liveReady: false,
  orderRouting: 'DISABLED',
  privateAuth: 'DEFERRED',
  manualRouting: 'DISABLED',
  blockingCount: 1,
  warningCount: 0,
  deferredCount: 6,
  retryableBlockingCount: 0,
  blockers: ['Real read-only provider: frontend mock mode cannot satisfy the Step 44 contract.'],
  warnings: [],
  deferred: [
    'Hyperliquid private API wallet authentication',
    'Private account snapshot',
    'Private order lifecycle',
    'Trading-service liveness',
    'Host clock synchronization',
    'Mock exchange adapter',
  ],
  requirements: [
    { id: 'provider-real', label: 'Real read-only provider', state: 'BLOCKED', requiredNow: true, requiredFor: ['DASHBOARD'], evidence: '/api/provider-status', detail: 'Frontend mock mode is intentionally not accepted as Step 44 real evidence.', retryable: false },
    { id: 'mock-exchange', label: 'Mock exchange adapter for full replay + dashboard', state: 'DEFERRED', requiredNow: false, requiredFor: ['REPLAY_MOCK'], evidence: 'future Venue Adapter boundary', detail: 'Future mock exchange must share the same adapter contract as the real venue.', retryable: false },
  ],
  checkedAt: 'mock',
  nextSafeStep: 'STEP_46_MANUAL_CONTROL_SAFE_ROUTING',
  futureReplayBoundary: 'HyperliquidAdapter and future MockExchangeAdapter share the canonical venue adapter contract.',
  note: 'Mock frontend mode is useful for UI work only and cannot satisfy the real Step 44 readiness contract.',
}

const shellStatus: ShellStatus = {
  mode: 'REPLAY',
  exchange: 'BINANCE',
  exchangeConnected: true,
  dataHealthy: true,
  tradingEnabled: true,
  reconciliation: 'CLEAN',
  readiness: 'READY',
  alertCount: 3,
  utcLabel: 'UTC 2026-09-23 14:32:18',
}

const positions: Position[] = [
  {
    asset: 'BTC', side: 'Long', quantity: 2.3456, quantityLabel: '2.3456', entryPrice: 34215, entryPriceLabel: '$34,215', currentPrice: 37420, currentPriceLabel: '$37,420',
    pnlUsd: 7517, pnlUsdLabel: '+$7,517', pnlPct: 9.1, pnlPctLabel: '+9.1%', currentWeightPct: 28.4, targetWeightPct: 28.0,
    targetQty: 2.3126, effectiveQty: 2.3456, localQty: 2.3456, exchangeQty: 2.3456, deltaQty: 0, status: 'ALIGNED',
  },
  {
    asset: 'ETH', side: 'Long', quantity: 12.4, quantityLabel: '12.40', entryPrice: 2312, entryPriceLabel: '$2,312', currentPrice: 2484, currentPriceLabel: '$2,484',
    pnlUsd: 2132, pnlUsdLabel: '+$2,132', pnlPct: 7.4, pnlPctLabel: '+7.4%', currentWeightPct: 22.1, targetWeightPct: 22.0,
    targetQty: 12.4, effectiveQty: 12.4, localQty: 12.4, exchangeQty: 12.4, deltaQty: 0, status: 'ALIGNED',
  },
  {
    asset: 'SOL', side: 'Long', quantity: 530, quantityLabel: '530.0', entryPrice: 124.5, entryPriceLabel: '$124.50', currentPrice: 138.2, currentPriceLabel: '$138.20',
    pnlUsd: 7261, pnlUsdLabel: '+$7,261', pnlPct: 11.0, pnlPctLabel: '+11.0%', currentWeightPct: 11.8, targetWeightPct: 12.0,
    targetQty: 530, effectiveQty: 530, localQty: 530, exchangeQty: 530, deltaQty: 0, status: 'ALIGNED',
  },
  {
    asset: 'LINK', side: 'Short', quantity: 2400, quantityLabel: '2,400', entryPrice: 14.82, entryPriceLabel: '$14.82', currentPrice: 14.21, currentPriceLabel: '$14.21',
    pnlUsd: 1464, pnlUsdLabel: '+$1,464', pnlPct: 4.1, pnlPctLabel: '+4.1%', currentWeightPct: 7.6, targetWeightPct: 8.0,
    targetQty: -2400, effectiveQty: -2400, localQty: -2400, exchangeQty: -2400, deltaQty: 0, status: 'ALIGNED',
  },
  {
    asset: 'BNB', side: 'Long', quantity: 18.5, quantityLabel: '18.50', entryPrice: 612.3, entryPriceLabel: '$612.30', currentPrice: 640.11, currentPriceLabel: '$640.11',
    pnlUsd: 514, pnlUsdLabel: '+$514', pnlPct: 4.6, pnlPctLabel: '+4.6%', currentWeightPct: 5.0, targetWeightPct: 5.0,
    targetQty: 18.5, effectiveQty: 18.5, localQty: 18.5, exchangeQty: 18.5, deltaQty: 0, status: 'ALIGNED',
  },
]

const overview: OverviewData = {
  stats: [
    { label: 'Total Equity', value: '$1,248,321', delta: '+2.36%', detail: '+$28,735 (7D)', trend: [7,8,8,9,9,10,11,10,12,13,12,14,15] },
    { label: 'Cash', value: '$312,448', delta: '+1.12%', detail: '+$3,461 (7D)', trend: [5,5,6,6,5,6,6,7,7,7,8,8,9] },
    { label: 'Unrealized PnL', value: '$86,242', delta: '+12.6%', detail: '+$9,612 (7D)', trend: [4,6,5,7,8,8,9,10,9,11,12,13,14] },
  ],
  equityCurve: labels.map((label, i) => ({ label, equity: equity[i], benchmark: benchmark[i] })),
  liveExpected: labels.map((label, i) => ({ label, equity: Math.round((equity[i]-690) * 0.14), benchmark: Math.round((benchmark[i]-650) * 0.12) })),
  positions,
  recentEvents: [
    { time: '14:31:52', event: 'BUY 0.50 BTC @ 37,420', source: 'Execution', severity: 'INFO' },
    { time: '14:28:11', event: 'Reconciliation check completed', source: 'System', severity: 'INFO' },
    { time: '14:24:03', event: 'Position deviation 0.18% within limits', source: 'Risk', severity: 'INFO' },
    { time: '14:20:17', event: 'SELL 1,200 SOL @ 137.90', source: 'Execution', severity: 'INFO' },
    { time: '14:17:36', event: 'High leverage attempt blocked', source: 'Risk', severity: 'WARN' },
    { time: '14:12:48', event: 'Market data gap detected (2s)', source: 'Market Data', severity: 'WARN' },
  ],
}

const positionsData: PositionsData = {
  totalEquity: '$1,248,321',
  activePositions: positions.length,
  grossExposure: '$935,873',
  grossExposurePct: '74.9%',
  netExposure: '$671,152',
  unrealizedPnl: '+$86,242',
  positions,
}

const reconciliationData: ReconciliationData = {
  status: 'CLEAN',
  lastChecked: '14:32:11 UTC',
  tolerance: '0.05% value / exchange lot tolerance',
  targetPortfolioValue: '$1,248,321',
  localPortfolioValue: '$1,248,318',
  exchangePortfolioValue: '$1,248,317',
  rows: positions.map(position => ({
    asset: position.asset,
    targetQty: position.targetQty,
    effectiveQty: position.effectiveQty,
    localQty: position.localQty,
    exchangeQty: position.exchangeQty,
    deltaQty: position.deltaQty,
    pendingQty: 0,
    status: position.status,
    explanation: 'Target, local execution state and exchange position agree within configured tolerance.',
  })),
  openOrders: [
    { orderId: 'ord-1842', asset: 'BTC', side: 'BUY', quantity: 0.12, remaining: 0.04, age: '18s', state: 'PARTIAL', exchangeId: 'BN-9841102' },
    { orderId: 'ord-1843', asset: 'SOL', side: 'SELL', quantity: 15, remaining: 15, age: '7s', state: 'NEW', exchangeId: 'BN-9841109' },
  ],
}



const pipelineData: PipelineData = {
  cycleLabel: 'Decision cycle · 2026-09-23 / 14:30 UTC',
  latestDecision: '14:30:00 UTC',
  universeSize: 20,
  activeSignals: 4,
  actionablePlans: 3,
  rows: [
    {
      cycleId: 'cycle-btc-20260923-1430', asset: 'BTC', decisionTime: '14:30:00', rank: 4, rsi: 84.3, signal: 'LONG', rawTargetPct: 15.2, volTargetPct: 11.8, approvedTargetPct: 10.0, currentWeightPct: 7.0, requiredDeltaPct: 3.0, plannedAction: 'BUY +3.0%', exchangeState: 'PARTIAL',
      stages: [
        { id:'market', label:'Market / Universe', value:'Rank #4', detail:'Top-20 liquidity universe · fresh', state:'OK' },
        { id:'indicators', label:'Indicators', value:'RSI(7) 84.3', detail:'SMA Volume(25) qualified', state:'OK' },
        { id:'signal', label:'Signal', value:'LONG', detail:'Persistent LEVEL intent', state:'CHANGED' },
        { id:'raw-target', label:'Raw target', value:'15.2%', detail:'Pre-risk portfolio target', state:'OK' },
        { id:'vol-target', label:'Vol targeting', value:'11.8%', detail:'Scaled by current volatility', state:'CHANGED' },
        { id:'risk', label:'Risk constraints', value:'10.0%', detail:'Asset cap reduced target', state:'CHANGED' },
        { id:'approved-target', label:'Approved target', value:'10.0%', detail:'Final risk-approved target', state:'OK' },
        { id:'current-position', label:'Current position', value:'7.0%', detail:'Effective position before plan', state:'OK' },
        { id:'delta', label:'Required delta', value:'+3.0%', detail:'Target minus effective state', state:'CHANGED' },
        { id:'order-plan', label:'Order plan', value:'BUY +3.0%', detail:'One order generated', state:'OK' },
        { id:'exchange', label:'Exchange', value:'PARTIAL', detail:'Order ord-1842 · 66.7% filled', state:'PENDING' },
      ],
    },
    {
      cycleId: 'cycle-eth-20260923-1430', asset: 'ETH', decisionTime: '14:30:00', rank: 7, rsi: 78.1, signal: 'LONG', rawTargetPct: 12.5, volTargetPct: 10.2, approvedTargetPct: 10.2, currentWeightPct: 10.1, requiredDeltaPct: 0.1, plannedAction: 'HOLD', exchangeState: 'NONE',
      stages: [
        { id:'market', label:'Market / Universe', value:'Rank #7', detail:'Top-20 liquidity universe · fresh', state:'OK' },
        { id:'indicators', label:'Indicators', value:'RSI(7) 78.1', detail:'Below entry threshold, existing signal persists', state:'OK' },
        { id:'signal', label:'Signal', value:'LONG', detail:'Persistent LEVEL state', state:'OK' },
        { id:'raw-target', label:'Raw target', value:'12.5%', detail:'Pre-risk portfolio target', state:'OK' },
        { id:'vol-target', label:'Vol targeting', value:'10.2%', detail:'Volatility scaling applied', state:'CHANGED' },
        { id:'risk', label:'Risk constraints', value:'10.2%', detail:'No additional cap modification', state:'OK' },
        { id:'approved-target', label:'Approved target', value:'10.2%', detail:'Final target', state:'OK' },
        { id:'current-position', label:'Current position', value:'10.1%', detail:'Effective current position', state:'OK' },
        { id:'delta', label:'Required delta', value:'+0.1%', detail:'Inside rebalance tolerance', state:'OK' },
        { id:'order-plan', label:'Order plan', value:'HOLD', detail:'No actionable order', state:'OK' },
        { id:'exchange', label:'Exchange', value:'NONE', detail:'No order submitted', state:'OK' },
      ],
    },
    {
      cycleId: 'cycle-sol-20260923-1430', asset: 'SOL', decisionTime: '14:30:00', rank: 9, rsi: 86.8, signal: 'LONG', rawTargetPct: 14.4, volTargetPct: 9.7, approvedTargetPct: 9.7, currentWeightPct: 12.0, requiredDeltaPct: -2.3, plannedAction: 'SELL -2.3%', exchangeState: 'FILLED',
      stages: [
        { id:'market', label:'Market / Universe', value:'Rank #9', detail:'Top-20 liquidity universe · fresh', state:'OK' },
        { id:'indicators', label:'Indicators', value:'RSI(7) 86.8', detail:'SMA Volume(25) qualified', state:'OK' },
        { id:'signal', label:'Signal', value:'LONG', detail:'Persistent LEVEL intent', state:'OK' },
        { id:'raw-target', label:'Raw target', value:'14.4%', detail:'Pre-risk target', state:'OK' },
        { id:'vol-target', label:'Vol targeting', value:'9.7%', detail:'High realized vol reduced target', state:'CHANGED' },
        { id:'risk', label:'Risk constraints', value:'9.7%', detail:'No further reduction', state:'OK' },
        { id:'approved-target', label:'Approved target', value:'9.7%', detail:'Final target', state:'OK' },
        { id:'current-position', label:'Current position', value:'12.0%', detail:'Effective current position', state:'OK' },
        { id:'delta', label:'Required delta', value:'-2.3%', detail:'Portfolio overweight vs approved target', state:'CHANGED' },
        { id:'order-plan', label:'Order plan', value:'SELL -2.3%', detail:'One reduction order generated', state:'OK' },
        { id:'exchange', label:'Exchange', value:'FILLED', detail:'Order ord-1839 completed', state:'OK' },
      ],
    },
    {
      cycleId: 'cycle-link-20260923-1430', asset: 'LINK', decisionTime: '14:30:00', rank: 12, rsi: 68.9, signal: 'FLAT', rawTargetPct: 0, volTargetPct: 0, approvedTargetPct: 0, currentWeightPct: -8.0, requiredDeltaPct: 8.0, plannedAction: 'BUY TO FLAT', exchangeState: 'PENDING',
      stages: [
        { id:'market', label:'Market / Universe', value:'Rank #12', detail:'Top-20 liquidity universe · fresh', state:'OK' },
        { id:'indicators', label:'Indicators', value:'RSI(7) 68.9', detail:'Below exit threshold 70', state:'CHANGED' },
        { id:'signal', label:'Signal', value:'FLAT', detail:'Exit transition from active state', state:'CHANGED' },
        { id:'raw-target', label:'Raw target', value:'0.0%', detail:'Strategy exited position', state:'CHANGED' },
        { id:'vol-target', label:'Vol targeting', value:'0.0%', detail:'No scaling required', state:'OK' },
        { id:'risk', label:'Risk constraints', value:'0.0%', detail:'No risk modification', state:'OK' },
        { id:'approved-target', label:'Approved target', value:'0.0%', detail:'Flat approved', state:'OK' },
        { id:'current-position', label:'Current position', value:'-8.0%', detail:'Short still effective until fill', state:'PENDING' },
        { id:'delta', label:'Required delta', value:'+8.0%', detail:'Buy required to flatten', state:'CHANGED' },
        { id:'order-plan', label:'Order plan', value:'BUY TO FLAT', detail:'Close order submitted', state:'OK' },
        { id:'exchange', label:'Exchange', value:'PENDING', detail:'Awaiting exchange fill', state:'PENDING' },
      ],
    },
  ],
  traces: {
    'cycle-btc-20260923-1430': {
      cycleId:'cycle-btc-20260923-1430', asset:'BTC',
      market:{ price:'$37,420.00', universe:'Top-20 SMA(Volume,25)', liquidityRank:'#4', freshness:'0.8s · HEALTHY' },
      strategy:{ rsi:'84.3', signal:'LONG / +1', transition:'LONG → LONG', intent:'Maintain long exposure' },
      portfolio:{ rawTarget:'15.2%', volTarget:'11.8%', riskChanges:['Asset cap: 11.8% → 10.0%'], approvedTarget:'10.0%' },
      planning:{ effectiveQty:'1.6400 BTC', pendingQty:'0.0800 BTC', delta:'+0.7026 BTC', stateRevision:'rev-88214' },
      execution:{ orderIds:['ord-1842'], side:'BUY', quantity:'0.7026 BTC', fillState:'PARTIAL · 0.4684 filled', fees:'$9.84', slippage:'+1.7 bps' },
      reconciliation:{ localQty:'2.1084 BTC', exchangeQty:'2.1084 BTC', status:'PENDING' },
      audit:{ correlationId:'corr-btc-20260923-1430-a91f', configVersion:'cfg-2026.09.23-07', mode:'REPLAY', exchange:'BINANCE', decisionTimestamp:'2026-09-23 14:30:00 UTC', executionTimestamp:'2026-09-23 14:30:02 UTC' },
    },
    'cycle-eth-20260923-1430': {
      cycleId:'cycle-eth-20260923-1430', asset:'ETH',
      market:{ price:'$2,484.00', universe:'Top-20 SMA(Volume,25)', liquidityRank:'#7', freshness:'0.8s · HEALTHY' },
      strategy:{ rsi:'78.1', signal:'LONG / +1', transition:'LONG → LONG', intent:'Maintain long exposure' },
      portfolio:{ rawTarget:'12.5%', volTarget:'10.2%', riskChanges:['No portfolio cap changes'], approvedTarget:'10.2%' },
      planning:{ effectiveQty:'12.4000 ETH', pendingQty:'0', delta:'+0.0400 ETH', stateRevision:'rev-88215' },
      execution:{ orderIds:[], side:'—', quantity:'—', fillState:'No actionable order', fees:'$0.00', slippage:'—' },
      reconciliation:{ localQty:'12.4000 ETH', exchangeQty:'12.4000 ETH', status:'ALIGNED' },
      audit:{ correlationId:'corr-eth-20260923-1430-f214', configVersion:'cfg-2026.09.23-07', mode:'REPLAY', exchange:'BINANCE', decisionTimestamp:'2026-09-23 14:30:00 UTC', executionTimestamp:'—' },
    },
    'cycle-sol-20260923-1430': {
      cycleId:'cycle-sol-20260923-1430', asset:'SOL',
      market:{ price:'$138.20', universe:'Top-20 SMA(Volume,25)', liquidityRank:'#9', freshness:'0.8s · HEALTHY' },
      strategy:{ rsi:'86.8', signal:'LONG / +1', transition:'LONG → LONG', intent:'Maintain long exposure' },
      portfolio:{ rawTarget:'14.4%', volTarget:'9.7%', riskChanges:['Vol target scaling: 14.4% → 9.7%'], approvedTarget:'9.7%' },
      planning:{ effectiveQty:'530.0000 SOL', pendingQty:'0', delta:'-101.6500 SOL', stateRevision:'rev-88216' },
      execution:{ orderIds:['ord-1839'], side:'SELL', quantity:'101.6500 SOL', fillState:'FILLED', fees:'$4.18', slippage:'-0.6 bps' },
      reconciliation:{ localQty:'428.3500 SOL', exchangeQty:'428.3500 SOL', status:'ALIGNED' },
      audit:{ correlationId:'corr-sol-20260923-1430-b705', configVersion:'cfg-2026.09.23-07', mode:'REPLAY', exchange:'BINANCE', decisionTimestamp:'2026-09-23 14:30:00 UTC', executionTimestamp:'2026-09-23 14:30:03 UTC' },
    },
    'cycle-link-20260923-1430': {
      cycleId:'cycle-link-20260923-1430', asset:'LINK',
      market:{ price:'$14.21', universe:'Top-20 SMA(Volume,25)', liquidityRank:'#12', freshness:'0.8s · HEALTHY' },
      strategy:{ rsi:'68.9', signal:'FLAT / 0', transition:'SHORT → FLAT', intent:'Exit position' },
      portfolio:{ rawTarget:'0.0%', volTarget:'0.0%', riskChanges:['No risk modification'], approvedTarget:'0.0%' },
      planning:{ effectiveQty:'-2400 LINK', pendingQty:'+2400 LINK', delta:'+2400 LINK', stateRevision:'rev-88217' },
      execution:{ orderIds:['ord-1844'], side:'BUY', quantity:'2400 LINK', fillState:'PENDING', fees:'Pending', slippage:'Pending' },
      reconciliation:{ localQty:'-2400 LINK', exchangeQty:'-2400 LINK', status:'PENDING' },
      audit:{ correlationId:'corr-link-20260923-1430-c880', configVersion:'cfg-2026.09.23-07', mode:'REPLAY', exchange:'BINANCE', decisionTimestamp:'2026-09-23 14:30:00 UTC', executionTimestamp:'Pending' },
    },
  },
}


const executionData: ExecutionData = {
  lastUpdated: '14:32:18 UTC',
  openOrders: 3,
  partialOrders: 1,
  pendingCancels: 1,
  filledOrders: 5,
  fillCount: 7,
  rejectCount: 1,
  avgSubmitLatencyMs: 42,
  avgFillLatencyMs: 318,
  avgSlippageBpsLabel: '-0.2 bps',
  submitLatencyP95Ms: 71,
  fillLatencyP95Ms: 622,
  bestSlippageBpsLabel: '-1.4 bps',
  worstSlippageBpsLabel: '+3.1 bps',
  totalFeesLabel: '$31.72',
  rejectRateLabel: '1.4%',
  orders: [
    {
      orderId: 'ord-1844', exchangeOrderId: 'bn-94710284', cycleId: 'cycle-link-20260923-1430', correlationId: 'corr-link-20260923-1430-c880', asset: 'LINK', side: 'BUY', quantity: 2400, filledQty: 0, remainingQty: 2400, state: 'NEW', age: '18s', expectedPriceLabel: '$14.21', avgFillPriceLabel: '—', feesLabel: '$0.00', slippageBps: 0, slippageBpsLabel: 'Pending', submitLatencyMs: 38, fillLatencyMs: null, submittedAt: '14:32:00.118', lastUpdateAt: '14:32:00.156',
      lifecycle: [
        { time: '14:32:00.101', state: 'PLANNED', detail: 'BUY 2,400 LINK generated to flatten approved target.', tone: 'info' },
        { time: '14:32:00.118', state: 'SUBMITTED', detail: 'Order handed to ExchangeGateway.', tone: 'info' },
        { time: '14:32:00.156', state: 'NEW', detail: 'Venue acknowledged order bn-94710284.', tone: 'warn' },
      ],
    },
    {
      orderId: 'ord-1842', exchangeOrderId: 'bn-94710071', cycleId: 'cycle-btc-20260923-1430', correlationId: 'corr-btc-20260923-1430-a91f', asset: 'BTC', side: 'BUY', quantity: 0.7026, filledQty: 0.4684, remainingQty: 0.2342, state: 'PARTIAL', age: '2m 04s', expectedPriceLabel: '$37,420.00', avgFillPriceLabel: '$37,426.36', feesLabel: '$9.84', slippageBps: 1.7, slippageBpsLabel: '+1.7 bps', submitLatencyMs: 45, fillLatencyMs: 286, submittedAt: '14:30:14.042', lastUpdateAt: '14:31:41.908',
      lifecycle: [
        { time: '14:30:14.002', state: 'PLANNED', detail: 'BUY 0.7026 BTC generated from +3.0% required delta.', tone: 'info' },
        { time: '14:30:14.042', state: 'SUBMITTED', detail: 'Order submitted to ExchangeGateway.', tone: 'info' },
        { time: '14:30:14.087', state: 'NEW', detail: 'Venue accepted the order.', tone: 'warn' },
        { time: '14:30:14.328', state: 'PARTIAL FILL', detail: '0.2400 BTC filled at $37,425.90.', tone: 'warn' },
        { time: '14:31:41.908', state: 'PARTIAL FILL', detail: 'Cumulative filled quantity reached 0.4684 BTC.', tone: 'warn' },
      ],
    },
    {
      orderId: 'ord-1841', exchangeOrderId: 'bn-94709902', cycleId: 'cycle-avax-20260923-1430', correlationId: 'corr-avax-20260923-1430-2441', asset: 'AVAX', side: 'SELL', quantity: 280, filledQty: 0, remainingQty: 280, state: 'PENDING_CANCEL', age: '3m 12s', expectedPriceLabel: '$41.82', avgFillPriceLabel: '—', feesLabel: '$0.00', slippageBps: 0, slippageBpsLabel: 'Pending', submitLatencyMs: 41, fillLatencyMs: null, submittedAt: '14:29:06.114', lastUpdateAt: '14:31:52.020',
      lifecycle: [
        { time: '14:29:06.071', state: 'PLANNED', detail: 'SELL 280 AVAX submitted for target reduction.', tone: 'info' },
        { time: '14:29:06.114', state: 'SUBMITTED', detail: 'Venue order created.', tone: 'info' },
        { time: '14:31:51.982', state: 'CANCEL REQUESTED', detail: 'Planner produced replacement after target revision.', tone: 'warn' },
        { time: '14:31:52.020', state: 'PENDING_CANCEL', detail: 'Awaiting venue cancel acknowledgement.', tone: 'warn' },
      ],
    },
    {
      orderId: 'ord-1839', exchangeOrderId: 'bn-94709461', cycleId: 'cycle-sol-20260923-1430', correlationId: 'corr-sol-20260923-1430-b705', asset: 'SOL', side: 'SELL', quantity: 101.65, filledQty: 101.65, remainingQty: 0, state: 'FILLED', age: '4m 44s', expectedPriceLabel: '$138.20', avgFillPriceLabel: '$138.19', feesLabel: '$4.18', slippageBps: -0.6, slippageBpsLabel: '-0.6 bps', submitLatencyMs: 39, fillLatencyMs: 214, submittedAt: '14:27:34.602', lastUpdateAt: '14:27:34.816',
      lifecycle: [
        { time: '14:27:34.561', state: 'PLANNED', detail: 'SELL 101.65 SOL generated from overweight position.', tone: 'info' },
        { time: '14:27:34.602', state: 'SUBMITTED', detail: 'Order submitted to venue.', tone: 'info' },
        { time: '14:27:34.816', state: 'FILLED', detail: 'Order fully filled in one venue execution.', tone: 'good' },
      ],
    },
    {
      orderId: 'ord-1838', exchangeOrderId: '', cycleId: 'cycle-atom-20260923-1427', correlationId: 'corr-atom-20260923-1427-d812', asset: 'ATOM', side: 'BUY', quantity: 850, filledQty: 0, remainingQty: 850, state: 'REJECTED', age: '5m 07s', expectedPriceLabel: '$8.52', avgFillPriceLabel: '—', feesLabel: '$0.00', slippageBps: 0, slippageBpsLabel: '—', submitLatencyMs: 54, fillLatencyMs: null, submittedAt: '14:27:11.209', lastUpdateAt: '14:27:11.263',
      lifecycle: [
        { time: '14:27:11.156', state: 'PLANNED', detail: 'BUY 850 ATOM generated by target delta.', tone: 'info' },
        { time: '14:27:11.209', state: 'SUBMITTED', detail: 'Request sent through ExchangeGateway.', tone: 'info' },
        { time: '14:27:11.263', state: 'REJECTED', detail: 'Venue rejected order: quantity precision invalid.', tone: 'bad' },
      ],
    },
    {
      orderId: 'ord-1836', exchangeOrderId: 'bn-94708741', cycleId: 'cycle-eth-20260923-1425', correlationId: 'corr-eth-20260923-1425-f001', asset: 'ETH', side: 'BUY', quantity: 1.8, filledQty: 1.8, remainingQty: 0, state: 'FILLED', age: '7m 18s', expectedPriceLabel: '$2,480.20', avgFillPriceLabel: '$2,480.42', feesLabel: '$2.68', slippageBps: 0.9, slippageBpsLabel: '+0.9 bps', submitLatencyMs: 36, fillLatencyMs: 344, submittedAt: '14:25:00.406', lastUpdateAt: '14:25:00.750',
      lifecycle: [
        { time: '14:25:00.370', state: 'PLANNED', detail: 'BUY 1.8 ETH generated from portfolio delta.', tone: 'info' },
        { time: '14:25:00.406', state: 'SUBMITTED', detail: 'Venue order created.', tone: 'info' },
        { time: '14:25:00.750', state: 'FILLED', detail: 'Order fully filled.', tone: 'good' },
      ],
    },
  ],
  partialFills: [
    { fillId: 'fill-7712', orderId: 'ord-1842', asset: 'BTC', quantity: 0.24, priceLabel: '$37,425.90', cumulativeLabel: '0.2400 / 0.7026', feesLabel: '$5.04', timestamp: '14:30:14.328' },
    { fillId: 'fill-7715', orderId: 'ord-1842', asset: 'BTC', quantity: 0.2284, priceLabel: '$37,426.84', cumulativeLabel: '0.4684 / 0.7026', feesLabel: '$4.80', timestamp: '14:31:41.908' },
  ],
  rejects: [
    { orderId: 'ord-1838', asset: 'ATOM', code: 'LOT_SIZE / -1013', reason: 'Requested quantity did not satisfy venue precision constraints.', remediation: 'Blocked from blind retry. Planner must normalize quantity and produce a new auditable order.' },
  ],
  replacements: [
    { asset: 'AVAX', originalOrderId: 'ord-1841', cancelRequestedAt: '14:31:51.982', canceledAt: 'Pending', replacementOrderId: 'ord-1845', replacementState: 'NEW' },
    { asset: 'BTC', originalOrderId: 'ord-1832', cancelRequestedAt: '14:23:20.011', canceledAt: '14:23:20.104', replacementOrderId: 'ord-1833', replacementState: 'FILLED' },
  ],
}


const riskData: RiskData = {
  riskState: 'PORTFOLIO WITHIN LIMITS',
  portfolioLimits: [
    { id: 'gross', label: 'Gross Exposure', currentLabel: '76.2%', limitLabel: '100.0%', utilizationPct: 76.2, state: 'OK' },
    { id: 'net', label: 'Net Exposure', currentLabel: '61.0%', limitLabel: '80.0%', utilizationPct: 76.3, state: 'OK' },
    { id: 'vol', label: 'Portfolio Volatility', currentLabel: '14.8%', limitLabel: '18.0%', utilizationPct: 82.2, state: 'WARN' },
    { id: 'drawdown', label: 'Drawdown', currentLabel: '4.1%', limitLabel: '12.0%', utilizationPct: 34.2, state: 'OK' },
    { id: 'cash', label: 'Cash Reserve', currentLabel: '25.0%', limitLabel: '≥ 15.0%', utilizationPct: 60.0, state: 'OK' },
    { id: 'positions', label: 'Active Positions', currentLabel: '5', limitLabel: '10', utilizationPct: 50.0, state: 'OK' },
  ],
  assetLimits: [
    { asset: 'BTC', currentWeightLabel: '28.4%', approvedWeightLabel: '28.0%', maxWeightLabel: '30.0%', utilizationPct: 94.7, state: 'WARN' },
    { asset: 'ETH', currentWeightLabel: '22.1%', approvedWeightLabel: '22.0%', maxWeightLabel: '30.0%', utilizationPct: 73.7, state: 'OK' },
    { asset: 'SOL', currentWeightLabel: '11.8%', approvedWeightLabel: '12.0%', maxWeightLabel: '15.0%', utilizationPct: 78.7, state: 'OK' },
    { asset: 'LINK', currentWeightLabel: '7.6%', approvedWeightLabel: '8.0%', maxWeightLabel: '10.0%', utilizationPct: 76.0, state: 'OK' },
    { asset: 'BNB', currentWeightLabel: '5.0%', approvedWeightLabel: '5.0%', maxWeightLabel: '10.0%', utilizationPct: 50.0, state: 'OK' },
  ],
  breaches: [
    { rule: 'BTC asset cap proximity', currentLabel: '28.4%', thresholdLabel: '30.0%', state: 'WARN', action: 'No block. New BTC target increases would be clipped by PortfolioRisk.' },
    { rule: 'Portfolio volatility proximity', currentLabel: '14.8%', thresholdLabel: '18.0%', state: 'WARN', action: 'Volatility scaling remains active; no pause required.' },
  ],
  safety: {
    tradingState: 'TRADING READY',
    reason: 'No hard risk or reconciliation blocker is active.',
    killSwitch: 'ARMED',
    lastRiskCheck: '14:32:12.884 UTC',
    riskRevision: 'risk-rev-1842',
  },
  exchangeAllocation: {
    assignedLabel: '$1,248,321',
    usedLabel: '$935,873',
    freeLabel: '$312,448',
    lockedLabel: '$42,180',
    lockedPct: 3.4,
  },
}

const marketDataData: MarketDataData = {
  source: 'historical-cmc',
  latestCompletedCandle: '2026-09-22 00:00 UTC',
  healthyAssets: 19,
  staleAssets: 1,
  totalAssets: 20,
  universeSize: 20,
  activeSignals: 5,
  availableSlots: 5,
  freshness: [
    { asset: 'BTC', lastCandle: '2026-09-22', ageLabel: '14h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'HEALTHY' },
    { asset: 'ETH', lastCandle: '2026-09-22', ageLabel: '14h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'HEALTHY' },
    { asset: 'SOL', lastCandle: '2026-09-22', ageLabel: '14h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'HEALTHY' },
    { asset: 'LINK', lastCandle: '2026-09-22', ageLabel: '14h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'HEALTHY' },
    { asset: 'BNB', lastCandle: '2026-09-22', ageLabel: '14h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'HEALTHY' },
    { asset: 'AVAX', lastCandle: '2026-09-21', ageLabel: '38h 32m', staleThresholdLabel: '36h', source: 'historical-cmc', state: 'STALE' },
  ],
  integrity: { missingCandles: 0, duplicateTimestamps: 0, gaps: 0, invalidRows: 0 },
  universe: [
    { rank: 1, asset: 'BTC', smaVolumeLabel: '$31.8B', rsi: 84.3, signal: 'LONG', signalState: 'ACTIVE', slotState: 'OCCUPIED', diagnostic: 'Entry condition active.', diagnosticTone: 'normal' },
    { rank: 2, asset: 'ETH', smaVolumeLabel: '$18.2B', rsi: 76.9, signal: 'LONG', signalState: 'PERSIST', slotState: 'OCCUPIED', diagnostic: 'Persistent long state.', diagnosticTone: 'normal' },
    { rank: 3, asset: 'SOL', smaVolumeLabel: '$4.6B', rsi: 72.1, signal: 'LONG', signalState: 'PERSIST', slotState: 'OCCUPIED', diagnostic: 'Persistent long state.', diagnosticTone: 'normal' },
    { rank: 4, asset: 'LINK', smaVolumeLabel: '$1.9B', rsi: 68.4, signal: 'FLAT', signalState: 'EXIT', slotState: 'FREE', diagnostic: 'Below exit threshold.', diagnosticTone: 'normal' },
    { rank: 5, asset: 'BNB', smaVolumeLabel: '$1.5B', rsi: 81.2, signal: 'LONG', signalState: 'ACTIVE', slotState: 'OCCUPIED', diagnostic: 'Entry condition active.', diagnosticTone: 'normal' },
    { rank: 6, asset: 'AVAX', smaVolumeLabel: '$920M', rsi: 82.8, signal: 'FLAT', signalState: 'BLOCKED', slotState: 'FREE', diagnostic: 'Stale data blocks candidate.', diagnosticTone: 'bad' },
    { rank: 7, asset: 'ATOM', smaVolumeLabel: '$610M', rsi: 80.6, signal: 'FLAT', signalState: 'REJECTED', slotState: 'FREE', diagnostic: 'Candidate qualified but risk/slot rules prevented activation.', diagnosticTone: 'warn' },
  ],
  candidateRejections: [
    { asset: 'AVAX', reason: 'STALE MARKET DATA', detail: 'Latest completed candle exceeds the configured freshness threshold.', severity: 'INVALID' },
    { asset: 'ATOM', reason: 'PORTFOLIO CONSTRAINT', detail: 'Qualifying-looking signal did not become active after downstream portfolio/risk checks.', severity: 'WARN' },
  ],
  strategySnapshot: {
    ranking: 'Top 20 by SMA Volume(25)',
    entryRule: 'RSI(7) > 80',
    exitRule: 'RSI(7) < 70',
    maxPositions: '10 persistent positions',
    semantics: 'LEVEL',
  },
}



const infrastructureData: InfrastructureData = {
  readiness: 'READY',
  readinessReason: 'All hard dependencies acceptable',
  lastUpdated: '14:32:18 UTC',
  vps: {
    cpuPct: 18,
    ramPct: 43,
    diskPct: 37,
    load1m: 0.62,
    networkRxLabel: '2.8 MB/s',
    networkTxLabel: '1.1 MB/s',
    uptimeLabel: '18d 07h',
    clockOffsetLabel: '+3.2 ms',
    clockSynced: true,
    state: 'HEALTHY',
  },
  containers: [
    { name: 'market-data', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 2.8, ramMb: 118, lastHeartbeat: '0.4s ago' },
    { name: 'strategy', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 1.7, ramMb: 96, lastHeartbeat: '0.6s ago' },
    { name: 'portfolio-risk', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 1.2, ramMb: 82, lastHeartbeat: '0.5s ago' },
    { name: 'order-planner', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 1.1, ramMb: 74, lastHeartbeat: '0.7s ago' },
    { name: 'execution-state', state: 'RUNNING', health: 'HEALTHY', restartCount: 1, uptimeLabel: '6d 03h', cpuPct: 1.9, ramMb: 104, lastHeartbeat: '0.3s ago' },
    { name: 'exchange-gateway', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 2.4, ramMb: 126, lastHeartbeat: '0.2s ago' },
    { name: 'postgres', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 3.8, ramMb: 312, lastHeartbeat: '0.3s ago' },
    { name: 'nats', state: 'RUNNING', health: 'HEALTHY', restartCount: 0, uptimeLabel: '18d 07h', cpuPct: 1.3, ramMb: 88, lastHeartbeat: '0.2s ago' },
  ],
  services: [
    { service: 'MarketData', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.604', lagLabel: '0.4s', health: 'HEALTHY' },
    { service: 'Strategy', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.412', lagLabel: '0.6s', health: 'HEALTHY' },
    { service: 'PortfolioRisk', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.493', lagLabel: '0.5s', health: 'HEALTHY' },
    { service: 'OrderPlanner', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.312', lagLabel: '0.7s', health: 'HEALTHY' },
    { service: 'ExecutionState', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.718', lagLabel: '0.3s', health: 'HEALTHY' },
    { service: 'ExchangeGateway', ready: true, mode: 'REPLAY', lastEvent: '14:32:17.806', lagLabel: '0.2s', health: 'HEALTHY' },
  ],
  postgres: { connected: true, latencyMs: 3.8, activeConnections: 17, maxConnections: 100, storageUsedLabel: '18.7 GB / 50 GB', storagePct: 37, persistenceState: 'Durable commits healthy', state: 'HEALTHY' },
  nats: { connected: true, streams: 7, consumers: 18, pendingMessages: 4, maxConsumerLag: 2, redeliveries: 1, ackHealthLabel: 'ACK flow healthy', state: 'HEALTHY' },
  exchange: { venue: 'BINANCE', connected: true, reconnectState: 'Stable', lastApiActivity: '14:32:17.810', lastWsActivity: '14:32:18.041', apiLatencyMs: 46, state: 'HEALTHY' },
  outbox: { pendingMessages: 2, oldestAgeLabel: '0.8s', lastPublished: '14:32:17.922', state: 'HEALTHY' },
  dependencies: [
    { component: 'MarketData', state: 'HEALTHY', reason: 'Fresh canonical data available', lastCheck: '0.4s ago' },
    { component: 'Strategy', state: 'HEALTHY', reason: 'Ready and processing current frontier', lastCheck: '0.6s ago' },
    { component: 'PortfolioRisk', state: 'HEALTHY', reason: 'No blocking risk breach', lastCheck: '0.5s ago' },
    { component: 'OrderPlanner', state: 'HEALTHY', reason: 'Planner ready', lastCheck: '0.7s ago' },
    { component: 'ExecutionState', state: 'HEALTHY', reason: 'Execution state durable and current', lastCheck: '0.3s ago' },
    { component: 'ExchangeGateway', state: 'HEALTHY', reason: 'Exchange connection healthy', lastCheck: '0.2s ago' },
    { component: 'PostgreSQL', state: 'HEALTHY', reason: 'Connected · persistence healthy', lastCheck: '0.3s ago' },
    { component: 'NATS / JetStream', state: 'HEALTHY', reason: 'Consumers within lag tolerance', lastCheck: '0.2s ago' },
    { component: 'Reconciliation', state: 'HEALTHY', reason: 'Closed loop CLEAN', lastCheck: '7s ago' },
    { component: 'Clock sync', state: 'HEALTHY', reason: 'Host offset within tolerance', lastCheck: '3s ago' },
  ],
}

const alertsAuditData: AlertsAuditData = {
  activeCritical: 1,
  activeWarnings: 3,
  acknowledged: 1,
  resolved24h: 8,
  acknowledgementAvailable: true,
  durableAlertHistoryAvailable: true,
  humanAuditAvailable: true,
  watchdogAvailable: true,
  watchdogState: 'HEALTHY',
  watchdogLastSweepAt: '2026-09-26T10:00:00Z',
  watchdogLastSuccessAt: '2026-09-26T10:00:00Z',
  durableEventCount: 12,
  durableLifecycleEvents: [],
  alerts: [
    { id: 'alert-2081', timestamp: '14:31:58 UTC', severity: 'CRITICAL', status: 'ACTIVE', service: 'Reconciliation', eventType: 'RECONCILIATION_BLOCKED', title: 'Reconciliation block simulated', detail: 'Mock alert demonstrating how a hard closed-loop mismatch remains sticky until resolved.', correlationId: 'corr-recon-2081', linkedContext: 'Open Reconciliation' },
    { id: 'alert-2079', timestamp: '14:27:11 UTC', severity: 'WARN', status: 'ACTIVE', service: 'ExchangeGateway', asset: 'ATOM', eventType: 'ORDER_REJECT', title: 'Order rejected by venue', detail: 'Quantity precision was not accepted. Remediation is pending planner normalization.', correlationId: 'corr-atom-20260923-1427-d812', linkedContext: 'Open Execution' },
    { id: 'alert-2077', timestamp: '14:24:42 UTC', severity: 'WARN', status: 'ACKNOWLEDGED', service: 'MarketData', eventType: 'STALE_DATA', title: 'Transient stale market data', detail: 'One asset exceeded the freshness warning threshold before recovering.', correlationId: 'corr-md-2077', linkedContext: 'Open Market Data' },
    { id: 'alert-2074', timestamp: '14:18:03 UTC', severity: 'WARN', status: 'ACTIVE', service: 'NATS', eventType: 'CONSUMER_LAG', title: 'Consumer lag elevated', detail: 'Execution consumer reached lag 8 before returning below the operational threshold.', linkedContext: 'Open Infrastructure' },
    { id: 'alert-2068', timestamp: '14:02:19 UTC', severity: 'INFO', status: 'RESOLVED', service: 'Infrastructure', eventType: 'RECOVERY', title: 'ExecutionState recovered', detail: 'Service heartbeat and durable state returned healthy after a controlled restart.', correlationId: 'corr-exec-recovery-2068', linkedContext: 'Open Infrastructure' },
    { id: 'alert-2059', timestamp: '13:44:10 UTC', severity: 'INFO', status: 'RESOLVED', service: 'Reconciliation', eventType: 'RECONCILIATION_CLEAN', title: 'Reconciliation returned CLEAN', detail: 'Local and exchange quantities converged inside configured tolerance.', correlationId: 'corr-recon-2059', linkedContext: 'Open Reconciliation' },
  ],
  audit: [
    { id: 'audit-991', timestamp: '14:28:12 UTC', actor: 'system', actorType: 'SYSTEM', action: 'RECONCILIATION_CHECK', target: 'portfolio', result: 'SUCCESS', correlationId: 'corr-recon-2079', detail: 'Periodic closed-loop verification completed.' },
    { id: 'audit-990', timestamp: '14:27:11 UTC', actor: 'system', actorType: 'SYSTEM', action: 'ORDER_REJECT_RECORDED', target: 'ATOM / ord-1838', result: 'SUCCESS', correlationId: 'corr-atom-20260923-1427-d812', detail: 'Venue rejection persisted with exchange reason and remediation state.' },
    { id: 'audit-989', timestamp: '14:20:04 UTC', actor: 'operator@example', actorType: 'OPERATOR', action: 'ALERT_ACKNOWLEDGE', target: 'alert-2077', result: 'SUCCESS', requestHash: 'sha256:7fd1…ca92', correlationId: 'corr-md-2077', detail: 'Operator acknowledged transient market-data warning.' },
    { id: 'audit-987', timestamp: '14:02:20 UTC', actor: 'system', actorType: 'SYSTEM', action: 'SERVICE_RECOVERY', target: 'ExecutionState', result: 'SUCCESS', correlationId: 'corr-exec-recovery-2068', detail: 'Readiness dependency changed from WARN to HEALTHY.' },
    { id: 'audit-984', timestamp: '13:51:44 UTC', actor: 'operator@example', actorType: 'OPERATOR', action: 'PAUSE_REQUEST_PREVIEW', target: 'trading', result: 'NOOP', requestHash: 'sha256:11a2…90ff', detail: 'Preview only. Step 06 does not send control commands.' },
    { id: 'audit-981', timestamp: '13:42:31 UTC', actor: 'system', actorType: 'SYSTEM', action: 'CONFIG_VERSION_OBSERVED', target: 'cfg-2026.09.23-07', result: 'SUCCESS', detail: 'Dashboard mock observed active configuration version.' },
  ],
}


const behaviourLabels = ['09:00', '09:30', '10:00', '10:30', '11:00', '11:30', '12:00', '12:30', '13:00', '13:30', '14:00', '14:30']
const makeBehaviourTrend = (live: number[], mean: number, tolerance: number) => behaviourLabels.map((label, index) => ({
  label,
  live: live[index],
  mean,
  lower: mean - tolerance,
  upper: mean + tolerance,
}))
const defaultDistribution = [2, 5, 11, 19, 31, 42, 51, 56, 52, 41, 28, 17, 9, 4, 2]

const liveVsExpectedData: LiveVsExpectedData = {
  contractVersion: 'step45-v1-mock',
  status: 'MOCK_ONLY',
  validated: false,
  projectionReady: false,
  observationMode: 'MOCK',
  baselineFingerprint: '0'.repeat(64),
  baselineObservationCount: 0,
  baselineExcludesLatest: true,
  overallClassification: 'NORMAL',
  anomalyCount: 0,
  anomalies: [],
  coverage: [
    { id: 'market-inputs', label: 'Canonical market / strategy-input behaviour', state: 'BLOCKED', source: 'mock', detail: 'Frontend mock data cannot validate the real Step 45 projection.' },
    { id: 'execution-distribution', label: 'Execution slippage / latency / rejects', state: 'DEFERRED', source: 'future versioned execution baseline', detail: 'Deferred in the real contract.' },
    { id: 'performance-accounting', label: 'PnL / equity / holding-time behaviour', state: 'DEFERRED', source: 'future runtime-owned accounting projection', detail: 'Deferred in the real contract.' },
    { id: 'accepted-replay', label: 'Accepted replay behavioural baseline', state: 'DEFERRED', source: 'future versioned replay artifact', detail: 'Deferred until the shared venue-adapter replay path exists.' },
  ],
  readOnly: true,
  privateAuth: 'DEFERRED',
  orderRouting: 'DISABLED',
  checkedAt: 'mock',
  baselineLabel: 'Accepted replay baseline',
  baselineWindow: 'Rolling historical distributions · mock until backend mapping',
  latestObservation: '14:30 UTC',
  metricCount: 10,
  normalCount: 6,
  elevatedCount: 2,
  abnormalCount: 2,
  criticalCount: 0,
  metrics: [
    {
      id: 'turnover', family: 'ACTIVITY', label: 'Portfolio Turnover', currentLabel: '18.7% / day', historicalMeanLabel: '12.4% / day', historicalRangeLabel: '8.1–16.9%', zScore: 1.9, classification: 'ELEVATED',
      description: 'Daily notional turnover is above the centre of its historical distribution, driven mainly by recent BTC and LINK target changes.',
      trend: makeBehaviourTrend([11.9, 12.8, 13.1, 14.2, 13.8, 15.1, 14.9, 16.0, 16.4, 17.1, 17.8, 18.7], 12.4, 4.5), distribution: defaultDistribution,
      contributingCycles: [
        { timestamp: '14:30:14', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: '+3.0% delta', deviationLabel: '+0.8σ' },
        { timestamp: '14:32:00', asset: 'LINK', cycleId: 'cycle-link-20260923-1430', valueLabel: '+2.4% delta', deviationLabel: '+0.6σ' },
        { timestamp: '14:27:34', asset: 'SOL', cycleId: 'cycle-sol-20260923-1430', valueLabel: '-1.6% delta', deviationLabel: '+0.3σ' },
      ],
    },
    {
      id: 'signal-frequency', family: 'ACTIVITY', label: 'Signal Frequency', currentLabel: '4.8 / day', historicalMeanLabel: '4.4 / day', historicalRangeLabel: '2.8–6.0', zScore: 0.5, classification: 'NORMAL',
      description: 'Number of strategy signal transitions remains close to the historical centre.',
      trend: makeBehaviourTrend([4.2, 4.5, 4.1, 4.6, 4.9, 4.4, 4.6, 4.2, 4.7, 4.8, 4.5, 4.8], 4.4, 1.6), distribution: [1,4,9,18,29,43,55,61,58,45,30,17,8,3,1],
      contributingCycles: [
        { timestamp: '14:30:00', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: 'LONG retained', deviationLabel: '+0.2σ' },
        { timestamp: '13:58:00', asset: 'ETH', cycleId: 'cycle-eth-20260923-1358', valueLabel: 'LONG retained', deviationLabel: '+0.1σ' },
      ],
    },
    {
      id: 'active-positions', family: 'ACTIVITY', label: 'Active Positions', currentLabel: '5', historicalMeanLabel: '5.4', historicalRangeLabel: '3–8', zScore: -0.3, classification: 'NORMAL',
      description: 'The current number of open strategy positions is typical for the historical baseline.',
      trend: makeBehaviourTrend([5,5,5,6,6,5,5,5,5,5,5,5], 5.4, 2.4), distribution: [2,7,16,29,44,55,58,51,39,24,12,5,2],
      contributingCycles: [
        { timestamp: '14:30:00', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: 'Position active', deviationLabel: '0.0σ' },
        { timestamp: '14:25:00', asset: 'ETH', cycleId: 'cycle-eth-20260923-1425', valueLabel: 'Position active', deviationLabel: '0.0σ' },
      ],
    },
    {
      id: 'holding-time', family: 'HOLDING', label: 'Average Holding Time', currentLabel: '2.7 days', historicalMeanLabel: '3.1 days', historicalRangeLabel: '2.2–4.0 days', zScore: -0.8, classification: 'NORMAL',
      description: 'Average holding duration is slightly shorter than baseline but remains inside the historical envelope.',
      trend: makeBehaviourTrend([3.2,3.1,3.0,3.2,3.0,2.9,2.8,2.9,2.8,2.7,2.8,2.7], 3.1, 0.9), distribution: [1,3,8,15,27,43,54,59,54,44,29,16,8,3,1],
      contributingCycles: [
        { timestamp: '13:42:18', asset: 'SOL', cycleId: 'cycle-sol-20260923-1342', valueLabel: '2.4 days', deviationLabel: '-0.4σ' },
        { timestamp: '12:18:51', asset: 'AVAX', cycleId: 'cycle-avax-20260923-1218', valueLabel: '2.1 days', deviationLabel: '-0.6σ' },
      ],
    },
    {
      id: 'portfolio-vol', family: 'RISK', label: 'Portfolio Volatility', currentLabel: '14.8%', historicalMeanLabel: '14.1%', historicalRangeLabel: '10.8–17.4%', zScore: 0.4, classification: 'NORMAL',
      description: 'Realized portfolio volatility is close to its historical baseline and below the configured risk limit.',
      trend: makeBehaviourTrend([13.7,13.8,14.0,14.2,14.0,14.3,14.6,14.5,14.7,14.8,14.7,14.8], 14.1, 3.3), distribution: defaultDistribution,
      contributingCycles: [
        { timestamp: '14:30:14', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: '28.4% weight', deviationLabel: '+0.2σ' },
        { timestamp: '14:25:00', asset: 'ETH', cycleId: 'cycle-eth-20260923-1425', valueLabel: '22.1% weight', deviationLabel: '+0.1σ' },
      ],
    },
    {
      id: 'gross-exposure', family: 'RISK', label: 'Gross Exposure', currentLabel: '76.2%', historicalMeanLabel: '72.8%', historicalRangeLabel: '58–87%', zScore: 0.5, classification: 'NORMAL',
      description: 'Absolute portfolio exposure remains within the central part of the historical operating range.',
      trend: makeBehaviourTrend([72,73,74,72,73,74,75,75,76,75,76,76.2], 72.8, 14.5), distribution: [1,4,10,20,34,48,57,60,54,41,27,15,7,3,1],
      contributingCycles: [
        { timestamp: '14:32:00', asset: 'LINK', cycleId: 'cycle-link-20260923-1430', valueLabel: '+2.4% target', deviationLabel: '+0.2σ' },
        { timestamp: '14:30:14', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: '+3.0% target', deviationLabel: '+0.2σ' },
      ],
    },
    {
      id: 'rolling-sharpe', family: 'PERFORMANCE', label: 'Rolling Sharpe', currentLabel: '1.42', historicalMeanLabel: '1.18', historicalRangeLabel: '0.35–1.95', zScore: 0.6, classification: 'NORMAL',
      description: 'Rolling risk-adjusted performance shape is inside its historical range. This is descriptive and not a forward return estimate.',
      trend: makeBehaviourTrend([1.10,1.14,1.20,1.18,1.25,1.29,1.31,1.34,1.36,1.39,1.40,1.42], 1.18, 0.8), distribution: [2,5,11,21,34,49,57,60,53,40,26,14,7,3,1],
      contributingCycles: [
        { timestamp: '14:02:19', asset: 'PORTFOLIO', cycleId: 'window-30d-1402', valueLabel: '1.40', deviationLabel: '+0.5σ' },
        { timestamp: '13:32:19', asset: 'PORTFOLIO', cycleId: 'window-30d-1332', valueLabel: '1.39', deviationLabel: '+0.5σ' },
      ],
    },
    {
      id: 'slippage', family: 'EXECUTION', label: 'Execution Slippage', currentLabel: '+1.4 bps', historicalMeanLabel: '+0.7 bps', historicalRangeLabel: '-0.8–2.2 bps', zScore: 1.1, classification: 'ELEVATED',
      description: 'Realized execution prices are modestly worse than the historical centre but remain inside the broad baseline envelope.',
      trend: makeBehaviourTrend([0.6,0.8,0.7,0.9,0.8,1.0,1.1,1.0,1.2,1.2,1.3,1.4], 0.7, 1.5), distribution: [1,4,10,19,31,46,57,61,55,42,28,16,8,3,1],
      contributingCycles: [
        { timestamp: '14:30:14', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: '+1.7 bps', deviationLabel: '+0.7σ' },
        { timestamp: '14:25:00', asset: 'ETH', cycleId: 'cycle-eth-20260923-1425', valueLabel: '+0.9 bps', deviationLabel: '+0.2σ' },
      ],
    },
    {
      id: 'fill-latency', family: 'EXECUTION', label: 'Fill Latency', currentLabel: '612 ms', historicalMeanLabel: '280 ms', historicalRangeLabel: '120–510 ms', zScore: 2.4, classification: 'ABNORMAL',
      description: 'Recent fill completion latency is outside the usual historical envelope. Network, venue state and order lifecycle context should be inspected before drawing conclusions.',
      trend: makeBehaviourTrend([245,270,265,310,288,340,360,390,420,475,540,612], 280, 230), distribution: [2,8,19,37,56,68,63,47,29,15,7,3,1],
      contributingCycles: [
        { timestamp: '14:31:41', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: '1,648 ms', deviationLabel: '+1.4σ' },
        { timestamp: '14:25:00', asset: 'ETH', cycleId: 'cycle-eth-20260923-1425', valueLabel: '344 ms', deviationLabel: '+0.3σ' },
        { timestamp: '14:27:34', asset: 'SOL', cycleId: 'cycle-sol-20260923-1430', valueLabel: '214 ms', deviationLabel: '-0.2σ' },
      ],
    },
    {
      id: 'entry-rsi', family: 'STRATEGY INPUTS', label: 'Entry RSI', currentLabel: '89.1 median', historicalMeanLabel: '84.2 median', historicalRangeLabel: '80.3–88.0', zScore: 2.2, classification: 'ABNORMAL',
      description: 'Recent entries are occurring at higher RSI levels than the historical entry distribution, while still respecting the configured entry rule.',
      trend: makeBehaviourTrend([83.1,83.7,84.2,84.6,85.0,85.5,86.0,86.8,87.4,88.1,88.7,89.1], 84.2, 3.8), distribution: [1,5,14,29,48,62,68,61,43,25,12,5,2],
      contributingCycles: [
        { timestamp: '14:30:00', asset: 'BTC', cycleId: 'cycle-btc-20260923-1430', valueLabel: 'RSI 91.4', deviationLabel: '+1.0σ' },
        { timestamp: '14:10:00', asset: 'LINK', cycleId: 'cycle-link-20260923-1410', valueLabel: 'RSI 89.7', deviationLabel: '+0.7σ' },
        { timestamp: '13:50:00', asset: 'SOL', cycleId: 'cycle-sol-20260923-1350', valueLabel: 'RSI 86.2', deviationLabel: '+0.3σ' },
      ],
    },
  ],
}


const manualControlData: ManualControlData = {
  contractVersion: 'step46-v1',
  routingContractReady: false,
  routingContractMode: 'MOCK_ONLY',
  executionBoundary: 'Mock UI only',
  exchangeConstraintsValidated: false,
  tradingControlSink: 'UNCONFIGURED',
  privateAuth: 'DEFERRED',
  orderLifecycle: 'DEFERRED',
  confirmationRequired: true,
  confirmationPhrase: 'CONFIRM_MANUAL_ROUTE',
  humanAuditAvailable: false,
  routeBlockers: ['MOCK_PROVIDER', 'MANUAL_RISK_CONTRACT_UNAVAILABLE', 'TRADING_CONTROL_SINK_UNCONFIGURED', 'PRIVATE_AUTH_DEFERRED', 'ORDER_LIFECYCLE_DEFERRED'],
  recentRouteAudits: [],
  mode: 'REPLAY',
  exchange: 'BINANCE',
  schemaLabel: 'asset,weight_pct',
  maxUploadSizeLabel: '256 KB',
  exampleCsv: 'asset,weight_pct\nBTC,28\nETH,22\nSOL,10\nLINK,6\nBNB,4\nCASH,30',
  requestHash: 'mock:sha256:9db8f0e1a71c…',
  previewRows: [
    { asset: 'BTC', currentWeightPct: 28.4, requestedWeightPct: 28.0, approvedWeightPct: 28.0, estimatedNotionalLabel: '-$4,993', deltaPct: -0.4 },
    { asset: 'ETH', currentWeightPct: 22.1, requestedWeightPct: 22.0, approvedWeightPct: 22.0, estimatedNotionalLabel: '-$1,248', deltaPct: -0.1 },
    { asset: 'SOL', currentWeightPct: 11.8, requestedWeightPct: 10.0, approvedWeightPct: 10.0, estimatedNotionalLabel: '-$22,470', deltaPct: -1.8 },
    { asset: 'LINK', currentWeightPct: 7.6, requestedWeightPct: 6.0, approvedWeightPct: 6.0, estimatedNotionalLabel: '-$19,973', deltaPct: -1.6 },
    { asset: 'BNB', currentWeightPct: 5.0, requestedWeightPct: 4.0, approvedWeightPct: 4.0, estimatedNotionalLabel: '-$12,483', deltaPct: -1.0 },
    { asset: 'CASH', currentWeightPct: 25.1, requestedWeightPct: 30.0, approvedWeightPct: 30.0, estimatedNotionalLabel: '+$61,168', deltaPct: 4.9 },
  ],
  validationIssues: [
    { severity: 'INFO', field: 'schema', message: 'Required columns asset and weight_pct are present.' },
    { severity: 'INFO', field: 'weights', message: 'Requested weights sum to 100.00%.' },
    { severity: 'WARN', field: 'execution', message: 'Order quantities and venue constraints are mock-only until the Dashboard API is connected.' },
  ],
  orderPreview: [
    { asset: 'BTC', action: 'SELL', deltaWeightPct: -0.4, estimatedNotionalLabel: '$4,993', estimatedFeeLabel: '$2.50', note: 'Mock estimate only' },
    { asset: 'ETH', action: 'SELL', deltaWeightPct: -0.1, estimatedNotionalLabel: '$1,248', estimatedFeeLabel: '$0.62', note: 'Mock estimate only' },
    { asset: 'SOL', action: 'SELL', deltaWeightPct: -1.8, estimatedNotionalLabel: '$22,470', estimatedFeeLabel: '$11.24', note: 'Mock estimate only' },
    { asset: 'LINK', action: 'SELL', deltaWeightPct: -1.6, estimatedNotionalLabel: '$19,973', estimatedFeeLabel: '$9.99', note: 'Mock estimate only' },
    { asset: 'BNB', action: 'SELL', deltaWeightPct: -1.0, estimatedNotionalLabel: '$12,483', estimatedFeeLabel: '$6.24', note: 'Mock estimate only' },
  ],
  estimatedFeesLabel: '$30.59',
  estimatedTurnoverLabel: '4.90%',
  auditActorLabel: 'local-ui / unauthenticated mock',
}

export const mockDashboardDataSource: DashboardDataSource = {
  async getProviderStatus() {
    await wait()
    return providerStatus
  },
  async getDiagnostics() {
    await wait(40)
    return diagnosticsData
  },
  async getSafetyGate() {
    await wait(40)
    return safetyGateData
  },
  async getGlobalReadiness() {
    await wait(40)
    return globalReadinessData
  },
  async getVenueFoundation() {
    await wait(40)
    return venueFoundationData
  },
  async getVenuePublic() {
    await wait(40)
    return venuePublicData
  },
  async getVenueSymbolMapping() {
    await wait(40)
    return venueSymbolMappingData
  },
  async getVenueTradingRules() {
    await wait(40)
    return venueTradingRulesData
  },
  async getSymbolRegistry() {
    await wait(40)
    return symbolRegistryData
  },
  async getLedger() {
    await wait(40)
    return ledgerData
  },
  async getShellStatus() {
    await wait()
    return shellStatus
  },
  async getOverview() {
    await wait(120)
    return overview
  },
  async getPositions() {
    await wait(100)
    return positionsData
  },
  async getReconciliation() {
    await wait(100)
    return reconciliationData
  },
  async getPipeline() {
    await wait(110)
    return pipelineData
  },
  async getExecution() {
    await wait(95)
    return executionData
  },
  async getRisk() {
    await wait(90)
    return riskData
  },
  async getMarketData() {
    await wait(90)
    return marketDataData
  },
  async getInfrastructure() {
    await wait(90)
    return infrastructureData
  },
  async getAlertsAudit() {
    await wait(90)
    return alertsAuditData
  },
  async getLiveVsExpected() {
    await wait(90)
    return liveVsExpectedData
  },
  async getManualControl() {
    await wait(80)
    return manualControlData
  },
  async previewManualControl() {
    await wait(120)
    return { ...manualControlData, validationPassed: true, backendAuthoritative: false, routeEnabled: false, riskCheckAvailable: false, sourceMode: 'MOCK' as const }
  },
  async routeManualControl() {
    await wait(90)
    return {
      contractVersion: 'step46-v1', status: 'BLOCKED', submitted: false, routeEnabled: false,
      confirmationAccepted: true, requestHash: manualControlData.requestHash, correlationId: 'mock-manual-route',
      actor: 'mock / OPERATOR', referenceTargetTimestamp: 'mock', checkedAt: new Date().toISOString(),
      blockers: ['MOCK_PROVIDER', 'MANUAL_RISK_CONTRACT_UNAVAILABLE', 'TRADING_CONTROL_SINK_UNCONFIGURED', 'PRIVATE_AUTH_DEFERRED', 'ORDER_LIFECYCLE_DEFERRED', 'GLOBAL_TRADING_READINESS_FALSE'], auditPersisted: false,
      note: 'Mock provider cannot route or persist a real operator-intent audit.',
    }
  },
}
