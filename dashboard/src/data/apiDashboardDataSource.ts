import type { DashboardDataSource } from './dashboardDataSource'
import { DashboardApiClient } from './apiClient'
import type {
  AlertsAuditData,
  DashboardDiagnostics,
  ExecutionData,
  InfrastructureData,
  LiveVsExpectedData,
  ManualControlData,
  ManualControlPreviewInput,
  ManualControlRouteInput,
  ManualControlRouteResult,
  MarketDataData,
  OverviewData,
  PipelineData,
  ProviderStatus,
  PositionsData,
  ReconciliationData,
  RiskData,
  ShellStatus,
  SafetyGateData,
  GlobalReadinessData,
  VenueFoundationData,
  VenuePublicData,
  VenueSymbolMappingData,
  VenueTradingRulesData,
  SymbolRegistryData,
  LedgerData,
} from '../types/dashboard'

export class ApiDashboardDataSource implements DashboardDataSource {
  constructor(private readonly client: DashboardApiClient) {}

  getProviderStatus() { return this.client.get<ProviderStatus>('/provider-status') }
  getDiagnostics() { return this.client.get<DashboardDiagnostics>('/diagnostics') }
  getSafetyGate() { return this.client.get<SafetyGateData>('/safety-gate') }
  getGlobalReadiness() { return this.client.get<GlobalReadinessData>('/global-readiness') }
  getVenueFoundation() { return this.client.get<VenueFoundationData>('/venue-foundation') }
  getVenuePublic() { return this.client.get<VenuePublicData>('/venue-public') }
  getVenueSymbolMapping() { return this.client.get<VenueSymbolMappingData>('/venue-symbol-map') }
  getVenueTradingRules() { return this.client.get<VenueTradingRulesData>('/venue-rules') }
  getSymbolRegistry() { return this.client.get<SymbolRegistryData>('/symbol-registry') }
  getLedger() { return this.client.get<LedgerData>('/ledger') }
  getShellStatus() { return this.client.get<ShellStatus>('/shell-status') }
  getOverview() { return this.client.get<OverviewData>('/overview') }
  getPositions() { return this.client.get<PositionsData>('/positions') }
  getReconciliation() { return this.client.get<ReconciliationData>('/reconciliation') }
  getPipeline() { return this.client.get<PipelineData>('/pipeline') }
  getExecution() { return this.client.get<ExecutionData>('/execution') }
  getRisk() { return this.client.get<RiskData>('/risk') }
  getMarketData() { return this.client.get<MarketDataData>('/market-data') }
  getInfrastructure() { return this.client.get<InfrastructureData>('/infrastructure') }
  getAlertsAudit() { return this.client.get<AlertsAuditData>('/alerts-audit') }
  getLiveVsExpected() { return this.client.get<LiveVsExpectedData>('/live-vs-expected') }
  getManualControl() { return this.client.get<ManualControlData>('/manual-control') }
  previewManualControl(input: ManualControlPreviewInput, csrfToken: string) { return this.client.post<ManualControlData>('/manual-control/preview', input, { 'X-CSRF-Token': csrfToken }) }
  routeManualControl(input: ManualControlRouteInput, csrfToken: string) { return this.client.post<ManualControlRouteResult>('/manual-control/route', input, { 'X-CSRF-Token': csrfToken }) }
}
