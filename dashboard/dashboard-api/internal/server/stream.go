package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	"control-dashboard-api/internal/provider"
)

const (
	streamHeartbeatInterval = 15 * time.Second
	streamFallbackInterval  = 30 * time.Second
	streamDebounceInterval  = 175 * time.Millisecond
)

type dashboardEventSubscriber interface {
	SubscribeDashboardEvents(context.Context, func(string)) error
}

type streamInvalidation struct {
	Resources []string `json:"resources"`
	Subjects  []string `json:"subjects,omitempty"`
	Reason    string   `json:"reason"`
	At        string   `json:"at"`
}

func (s *Server) stream(w http.ResponseWriter, r *http.Request) {
	select {
	case s.streamSlots <- struct{}{}:
		s.metrics.sseActive.Add(1)
		defer func() {
			<-s.streamSlots
			s.metrics.sseActive.Add(-1)
		}()
	default:
		s.metrics.sseRejected.Add(1)
		w.Header().Set("Retry-After", "3")
		s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "dashboard SSE capacity reached; retry shortly"})
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "streaming is not supported by this response writer"})
		return
	}
	value, ok := sessionFromContext(r.Context())
	if !ok {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}
	cookie, err := r.Cookie("cd_session")
	if err != nil || cookie.Value == "" {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}

	// The server-wide write timeout protects ordinary API calls. SSE is a
	// long-lived response, so clear the deadline only for this authenticated
	// request.
	_ = http.NewResponseController(w).SetWriteDeadline(time.Time{})
	w.Header().Set("Content-Type", "text/event-stream; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.Header().Set("X-Dashboard-Stream", "sse-nats-step30")
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprint(w, "retry: 3000\n\n")

	health := s.provider.Health(r.Context())
	if err := writeSSEEvent(w, "connected", map[string]any{
		"version":                s.cfg.Version,
		"provider":               health.Name,
		"mode":                   health.Mode,
		"transport":              "SSE",
		"source":                 streamSourceLabel(s.provider),
		"heartbeatSeconds":       int(streamHeartbeatInterval / time.Second),
		"fallbackRefreshSeconds": int(streamFallbackInterval / time.Second),
		"at":                     time.Now().UTC().Format(time.RFC3339Nano),
	}); err != nil {
		return
	}
	flusher.Flush()

	// Refresh currently mounted resources once when the stream becomes live.
	if err := writeSSEEvent(w, "invalidate", streamInvalidation{
		Resources: []string{"*"},
		Reason:    "stream-connected",
		At:        time.Now().UTC().Format(time.RFC3339Nano),
	}); err != nil {
		return
	}
	flusher.Flush()

	subjectCh := make(chan string, 128)
	if source, ok := s.provider.(dashboardEventSubscriber); ok {
		go func() {
			_ = source.SubscribeDashboardEvents(r.Context(), func(subject string) {
				select {
				case subjectCh <- subject:
				case <-r.Context().Done():
				default:
					// A burst only causes a later broad fallback refresh; dropping an
					// invalidation must never block the trading runtime or NATS reader.
				}
			})
			close(subjectCh)
		}()
	} else {
		close(subjectCh)
	}

	heartbeat := time.NewTicker(streamHeartbeatInterval)
	defer heartbeat.Stop()
	fallback := time.NewTicker(streamFallbackInterval)
	defer fallback.Stop()

	expiresIn := time.Until(value.ExpiresAt)
	if expiresIn < 0 {
		expiresIn = 0
	}
	sessionExpiry := time.NewTimer(expiresIn)
	defer sessionExpiry.Stop()

	pendingSubjects := map[string]struct{}{}
	pendingResources := map[string]struct{}{}
	var debounceTimer *time.Timer
	var debounce <-chan time.Time

	flushPending := func() error {
		if len(pendingResources) == 0 {
			return nil
		}
		resources := sortedKeys(pendingResources)
		subjects := sortedKeys(pendingSubjects)
		pendingSubjects = map[string]struct{}{}
		pendingResources = map[string]struct{}{}
		if err := writeSSEEvent(w, "invalidate", streamInvalidation{
			Resources: resources,
			Subjects:  subjects,
			Reason:    "nats",
			At:        time.Now().UTC().Format(time.RFC3339Nano),
		}); err != nil {
			return err
		}
		flusher.Flush()
		return nil
	}

	for {
		select {
		case <-r.Context().Done():
			return
		case <-sessionExpiry.C:
			_ = writeSSEEvent(w, "auth-expired", map[string]string{"reason": "session expired"})
			flusher.Flush()
			return
		case <-heartbeat.C:
			if _, valid := s.sessions.get(cookie.Value); !valid {
				_ = writeSSEEvent(w, "auth-expired", map[string]string{"reason": "session invalidated"})
				flusher.Flush()
				return
			}
			if _, err := fmt.Fprintf(w, ": heartbeat %s\n\n", time.Now().UTC().Format(time.RFC3339Nano)); err != nil {
				return
			}
			flusher.Flush()
		case <-fallback.C:
			if _, valid := s.sessions.get(cookie.Value); !valid {
				return
			}
			if err := writeSSEEvent(w, "invalidate", streamInvalidation{
				Resources: []string{"*"},
				Reason:    "fallback-refresh",
				At:        time.Now().UTC().Format(time.RFC3339Nano),
			}); err != nil {
				return
			}
			flusher.Flush()
		case subject, open := <-subjectCh:
			if !open {
				subjectCh = nil
				continue
			}
			pendingSubjects[subject] = struct{}{}
			for _, resource := range dashboardResourcesForSubject(subject) {
				pendingResources[resource] = struct{}{}
			}
			if debounceTimer == nil {
				debounceTimer = time.NewTimer(streamDebounceInterval)
				debounce = debounceTimer.C
			}
		case <-debounce:
			if err := flushPending(); err != nil {
				return
			}
			debounceTimer = nil
			debounce = nil
		}
	}
}

func writeSSEEvent(w http.ResponseWriter, event string, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintf(w, "event: %s\ndata: %s\n\n", event, data)
	return err
}

func streamSourceLabel(p provider.Provider) string {
	if _, ok := p.(dashboardEventSubscriber); ok {
		return "NATS invalidations + 30s fail-safe refresh"
	}
	return "30s fail-safe refresh"
}

func sortedKeys(values map[string]struct{}) []string {
	result := make([]string, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	sort.Strings(result)
	return result
}

// dashboardResourcesForSubject intentionally returns invalidations rather than
// data payloads. The browser still retrieves each read model from the normal
// authenticated REST endpoint, so NATS never becomes a browser-facing data
// contract and the dashboard remains independent from trading execution.
func dashboardResourcesForSubject(subject string) []string {
	switch subject {
	case "simulation.step58.generation":
		return []string{"*"}
	case natsdiag.SubjectMarketDataRelease, natsdiag.SubjectMarketDataUpdated, natsdiag.SubjectMarketSliceClosed, natsdiag.SubjectMarketSliceSnapshot:
		return []string{"shell-status", "overview", "market-data", "live-vs-expected", "pipeline", "alerts-audit"}
	case natsdiag.SubjectStrategyIntents:
		return []string{"shell-status", "overview", "pipeline", "risk", "alerts-audit"}
	case natsdiag.SubjectDecisionBatch:
		return []string{"shell-status", "overview", "pipeline", "risk", "alerts-audit"}
	case natsdiag.SubjectOrderPlanningRequest, natsdiag.SubjectOrderPlan, natsdiag.SubjectNotionalOrderPlanningRequest, natsdiag.SubjectNotionalOrderPlan:
		return []string{"overview", "pipeline", "execution", "alerts-audit"}
	case natsdiag.SubjectSubmitOrder, natsdiag.SubjectCancelOrder:
		return []string{"shell-status", "overview", "pipeline", "execution", "alerts-audit"}
	case natsdiag.SubjectOrderUpdate, natsdiag.SubjectFill:
		return []string{"shell-status", "overview", "positions", "reconciliation", "pipeline", "execution", "alerts-audit"}
	case natsdiag.SubjectAccountSnapshot:
		return []string{"shell-status", "overview", "positions", "reconciliation", "pipeline", "risk", "alerts-audit"}
	case natsdiag.SubjectExecutionCycleComplete:
		return []string{"shell-status", "overview", "positions", "reconciliation", "pipeline", "execution", "alerts-audit"}
	case natsdiag.SubjectExchangeSnapshot:
		return []string{"shell-status", "overview", "positions", "reconciliation", "alerts-audit"}
	default:
		return []string{"shell-status"}
	}
}
