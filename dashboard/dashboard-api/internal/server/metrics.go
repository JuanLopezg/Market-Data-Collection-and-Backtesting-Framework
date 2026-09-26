package server

import (
	"bufio"
	"net"
	"net/http"
	"sync/atomic"
	"time"
)

type runtimeMetrics struct {
	startedAt         time.Time
	requests          atomic.Uint64
	serverErrors      atomic.Uint64
	activeRequests    atomic.Int64
	maxActiveRequests atomic.Int64
	totalLatencyNanos atomic.Uint64
	latencySamples    atomic.Uint64
	resourceTimeouts  atomic.Uint64
	sseActive         atomic.Int64
	sseRejected       atomic.Uint64
}

func newRuntimeMetrics() *runtimeMetrics {
	return &runtimeMetrics{startedAt: time.Now().UTC()}
}

func (m *runtimeMetrics) requestStarted() {
	m.requests.Add(1)
	active := m.activeRequests.Add(1)
	for {
		peak := m.maxActiveRequests.Load()
		if active <= peak || m.maxActiveRequests.CompareAndSwap(peak, active) {
			break
		}
	}
}

func (m *runtimeMetrics) requestFinished(status int, duration time.Duration, includeLatency bool) {
	m.activeRequests.Add(-1)
	if status >= http.StatusInternalServerError {
		m.serverErrors.Add(1)
	}
	if includeLatency {
		m.totalLatencyNanos.Add(uint64(duration))
		m.latencySamples.Add(1)
	}
}

func (m *runtimeMetrics) meanLatencyMs() float64 {
	samples := m.latencySamples.Load()
	if samples == 0 {
		return 0
	}
	return float64(m.totalLatencyNanos.Load()) / float64(samples) / float64(time.Millisecond)
}

type statusResponseWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusResponseWriter) WriteHeader(status int) {
	if w.status == 0 {
		w.status = status
	}
	w.ResponseWriter.WriteHeader(status)
}

func (w *statusResponseWriter) Write(body []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	return w.ResponseWriter.Write(body)
}

func (w *statusResponseWriter) Flush() {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	if flusher, ok := w.ResponseWriter.(http.Flusher); ok {
		flusher.Flush()
	}
}

func (w *statusResponseWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

func (w *statusResponseWriter) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	hijacker, ok := w.ResponseWriter.(http.Hijacker)
	if !ok {
		return nil, nil, http.ErrNotSupported
	}
	return hijacker.Hijack()
}
