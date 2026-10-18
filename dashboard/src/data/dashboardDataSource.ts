import type { AlertAcknowledgementInput, AlertAcknowledgementResult, AlertsAuditData, DashboardDiagnostics, ExecutionData, InfrastructureData, LiveVsExpectedData, ManualControlData, ManualControlPreviewInput, ManualControlRouteInput, ManualControlRouteResult, MarketDataData, OverviewData, PipelineData, PositionsData, ReconciliationData, RiskData, ShellStatus, ProviderStatus, SafetyGateData, GlobalReadinessData, SymbolMapData, LedgerData, HostHistoryData, HostHistoryQuery } from '../types/dashboard'

export interface DashboardDataSource {
  getProviderStatus(): Promise<ProviderStatus>
  getDiagnostics(): Promise<DashboardDiagnostics>
  getSafetyGate(): Promise<SafetyGateData>
  getGlobalReadiness(): Promise<GlobalReadinessData>
  getSymbolMap(): Promise<SymbolMapData>
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
  getHostHistory(query: HostHistoryQuery): Promise<HostHistoryData>
  getAlertsAudit(): Promise<AlertsAuditData>
  acknowledgeAlert(input: AlertAcknowledgementInput, csrfToken: string): Promise<AlertAcknowledgementResult>
  getLiveVsExpected(): Promise<LiveVsExpectedData>
  getManualControl(): Promise<ManualControlData>
  previewManualControl(input: ManualControlPreviewInput, csrfToken: string): Promise<ManualControlData>
  routeManualControl(input: ManualControlRouteInput, csrfToken: string): Promise<ManualControlRouteResult>
}
