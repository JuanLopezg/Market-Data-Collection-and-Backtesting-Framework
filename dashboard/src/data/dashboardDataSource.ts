import type { AlertsAuditData, DashboardDiagnostics, ExecutionData, InfrastructureData, LiveVsExpectedData, ManualControlData, ManualControlPreviewInput, ManualControlRouteInput, ManualControlRouteResult, MarketDataData, OverviewData, PipelineData, PositionsData, ReconciliationData, RiskData, ShellStatus, ProviderStatus, SafetyGateData, GlobalReadinessData, VenueFoundationData, VenuePublicData, VenueSymbolMappingData, VenueTradingRulesData, SymbolRegistryData, LedgerData } from '../types/dashboard'

export interface DashboardDataSource {
  getProviderStatus(): Promise<ProviderStatus>
  getDiagnostics(): Promise<DashboardDiagnostics>
  getSafetyGate(): Promise<SafetyGateData>
  getGlobalReadiness(): Promise<GlobalReadinessData>
  getVenueFoundation(): Promise<VenueFoundationData>
  getVenuePublic(): Promise<VenuePublicData>
  getVenueSymbolMapping(): Promise<VenueSymbolMappingData>
  getVenueTradingRules(): Promise<VenueTradingRulesData>
  getSymbolRegistry(): Promise<SymbolRegistryData>
  getLedger(): Promise<LedgerData>
  getShellStatus(): Promise<ShellStatus>
  getOverview(): Promise<OverviewData>
  getPositions(): Promise<PositionsData>
  getReconciliation(): Promise<ReconciliationData>
  getPipeline(): Promise<PipelineData>
  getExecution(): Promise<ExecutionData>
  getRisk(): Promise<RiskData>
  getMarketData(): Promise<MarketDataData>
  getInfrastructure(): Promise<InfrastructureData>
  getAlertsAudit(): Promise<AlertsAuditData>
  getLiveVsExpected(): Promise<LiveVsExpectedData>
  getManualControl(): Promise<ManualControlData>
  previewManualControl(input: ManualControlPreviewInput, csrfToken: string): Promise<ManualControlData>
  routeManualControl(input: ManualControlRouteInput, csrfToken: string): Promise<ManualControlRouteResult>
}
