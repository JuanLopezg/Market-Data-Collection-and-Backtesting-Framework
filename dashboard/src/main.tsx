import React from 'react'
import ReactDOM from 'react-dom/client'
import { BrowserRouter, Navigate, Route, Routes } from 'react-router-dom'
import { ProtectedRoute } from './components/ProtectedRoute'
import { AppShell } from './layout/AppShell'
import { AlertsAuditPage } from './pages/AlertsAuditPage'
import { ExecutionPage } from './pages/ExecutionPage'
import { InfrastructurePage } from './pages/InfrastructurePage'
import { LiveVsExpectedPage } from './pages/LiveVsExpectedPage'
import { LoginPage } from './pages/LoginPage'
import { ManualControlPage } from './pages/ManualControlPage'
import { MarketDataPage } from './pages/MarketDataPage'
import { OverviewPage } from './pages/OverviewPage'
import { PipelinePage } from './pages/PipelinePage'
import { PositionsPage } from './pages/PositionsPage'
import { ReconciliationPage } from './pages/ReconciliationPage'
import { RiskPage } from './pages/RiskPage'
import { AuthProvider } from './providers/AuthProvider'
import { DashboardDataSourceProvider } from './providers/DashboardDataSourceProvider'
import './styles/global.css'

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <BrowserRouter>
      <AuthProvider>
        <Routes>
          <Route path="/login" element={<LoginPage />} />
          <Route element={<ProtectedRoute />}>
            <Route element={<DashboardDataSourceProvider><AppShell /></DashboardDataSourceProvider>}>
              <Route index element={<OverviewPage />} />
              <Route path="positions" element={<PositionsPage />} />
              <Route path="reconciliation" element={<ReconciliationPage />} />
              <Route path="pipeline" element={<PipelinePage />} />
              <Route path="execution" element={<ExecutionPage />} />
              <Route path="risk" element={<RiskPage />} />
              <Route path="market-data" element={<MarketDataPage />} />
              <Route path="infrastructure" element={<InfrastructurePage />} />
              <Route path="manual-control" element={<ManualControlPage />} />
              <Route path="alerts-audit" element={<AlertsAuditPage />} />
              <Route path="live-vs-expected" element={<LiveVsExpectedPage />} />
            </Route>
          </Route>
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </AuthProvider>
    </BrowserRouter>
  </React.StrictMode>,
)
