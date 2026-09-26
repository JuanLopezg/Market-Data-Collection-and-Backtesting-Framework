package server

import (
	"net/http"
	"runtime"
	"time"
)

func (s *Server) diagnostics(w http.ResponseWriter, r *http.Request) {
	var mem runtime.MemStats
	runtime.ReadMemStats(&mem)
	requests := s.metrics.requests.Load()
	serverErrors := s.metrics.serverErrors.Load()
	errorRate := 0.0
	if requests > 0 {
		errorRate = float64(serverErrors) / float64(requests) * 100
	}

	health := s.provider.Health(r.Context())
	s.writeJSON(w, http.StatusOK, map[string]any{
		"version":       s.cfg.Version,
		"startedAt":     s.metrics.startedAt.Format(time.RFC3339),
		"uptimeSeconds": int64(time.Since(s.metrics.startedAt).Seconds()),
		"provider": map[string]any{
			"name":  health.Name,
			"mode":  health.Mode,
			"ready": health.Ready,
		},
		"http": map[string]any{
			"requests":           requests,
			"activeRequests":     s.metrics.activeRequests.Load(),
			"maxActiveRequests":  s.metrics.maxActiveRequests.Load(),
			"serverErrors":       serverErrors,
			"serverErrorRatePct": errorRate,
			"meanLatencyMs":      s.metrics.meanLatencyMs(),
			"resourceTimeouts":   s.metrics.resourceTimeouts.Load(),
			"resourceTimeoutMs":  s.cfg.ResourceTimeout.Milliseconds(),
		},
		"sse": map[string]any{
			"activeClients": s.metrics.sseActive.Load(),
			"maxClients":    s.cfg.SSEMaxClients,
			"rejected":      s.metrics.sseRejected.Load(),
		},
		"runtime": map[string]any{
			"goroutines":  runtime.NumGoroutine(),
			"allocBytes":  mem.Alloc,
			"sysBytes":    mem.Sys,
			"heapObjects": mem.HeapObjects,
		},
	})
}
