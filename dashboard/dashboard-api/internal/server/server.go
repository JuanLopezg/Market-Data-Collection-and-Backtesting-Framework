package server

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"sort"
	"strings"
	"time"

	"control-dashboard-api/internal/integration/catalog"
	"control-dashboard-api/internal/integration/probe"
	"control-dashboard-api/internal/manualaudit"
	"control-dashboard-api/internal/provider"
)

type Config struct {
	Addr            string
	Version         string
	Logger          *slog.Logger
	Users           []AuthUser
	SessionTTL      time.Duration
	CookieSecure    bool
	DemoAuthAllowed bool
	Provider        provider.Provider
	ResourceTimeout time.Duration
	SSEMaxClients   int
	ManualAuditDir  string
}

type Server struct {
	cfg          Config
	provider     provider.Provider
	users        map[string]AuthUser
	sessions     *sessionStore
	loginLimiter *loginLimiter
	metrics      *runtimeMetrics
	streamSlots  chan struct{}
	manualAudit  *manualaudit.Store
	handler      http.Handler
}

func New(cfg Config) (*Server, error) {
	if cfg.Addr == "" {
		cfg.Addr = ":8080"
	}
	if cfg.Version == "" {
		cfg.Version = "0.46.0"
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.SessionTTL <= 0 {
		cfg.SessionTTL = 8 * time.Hour
	}
	if cfg.ResourceTimeout <= 0 {
		cfg.ResourceTimeout = 7 * time.Second
	}
	if cfg.SSEMaxClients <= 0 {
		cfg.SSEMaxClients = 16
	}
	if len(cfg.Users) == 0 {
		return nil, fmt.Errorf("authentication requires at least one configured user")
	}
	if cfg.Provider == nil {
		return nil, fmt.Errorf("dashboard data provider is required")
	}

	users := make(map[string]AuthUser, len(cfg.Users))
	for _, user := range cfg.Users {
		user.Username = strings.TrimSpace(user.Username)
		if user.Username == "" || user.Password == "" {
			return nil, fmt.Errorf("authentication user has empty username or password")
		}
		if user.Role != RoleViewer && user.Role != RoleOperator {
			return nil, fmt.Errorf("authentication user %q has unsupported role %q", user.Username, user.Role)
		}
		if _, exists := users[user.Username]; exists {
			return nil, fmt.Errorf("duplicate authentication username %q", user.Username)
		}
		users[user.Username] = user
	}

	s := &Server{
		cfg:          cfg,
		provider:     cfg.Provider,
		users:        users,
		sessions:     newSessionStore(cfg.SessionTTL),
		loginLimiter: newLoginLimiter(),
		metrics:      newRuntimeMetrics(),
		streamSlots:  make(chan struct{}, cfg.SSEMaxClients),
		manualAudit:  manualaudit.New(cfg.ManualAuditDir),
	}
	s.handler = s.routes()
	return s, nil
}

func (s *Server) Handler() http.Handler { return s.handler }

func (s *Server) HTTPServer() *http.Server {
	return &http.Server{
		Addr:              s.cfg.Addr,
		Handler:           s.handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}
}

func (s *Server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/health", s.health)
	mux.HandleFunc("GET /api/readiness", s.readiness)
	mux.HandleFunc("POST /api/auth/login", s.login)
	mux.Handle("GET /api/auth/me", s.authRequired(http.HandlerFunc(s.me)))
	mux.Handle("GET /api/provider-status", s.authRequired(http.HandlerFunc(s.providerStatus)))
	mux.Handle("GET /api/diagnostics", s.authRequired(http.HandlerFunc(s.diagnostics)))
	mux.Handle("GET /api/safety-gate", s.authRequired(http.HandlerFunc(s.safetyGate)))
	mux.Handle("GET /api/global-readiness", s.authRequired(http.HandlerFunc(s.globalReadiness)))
	mux.Handle("GET /api/venue-foundation", s.authRequired(http.HandlerFunc(s.venueFoundation)))
	mux.Handle("GET /api/venue-public", s.authRequired(http.HandlerFunc(s.venuePublic)))
	mux.Handle("GET /api/venue-symbol-map", s.authRequired(http.HandlerFunc(s.venueSymbolMap)))
	mux.Handle("GET /api/venue-rules", s.authRequired(http.HandlerFunc(s.venueRules)))
	mux.Handle("GET /api/symbol-registry", s.authRequired(http.HandlerFunc(s.symbolRegistry)))
	mux.Handle("GET /api/ledger", s.authRequired(http.HandlerFunc(s.ledger)))
	mux.Handle("GET /api/stream", s.authRequired(http.HandlerFunc(s.stream)))
	mux.Handle("GET /api/integration-catalog", s.authRequired(http.HandlerFunc(s.integrationCatalog)))
	mux.Handle("GET /api/source-status", s.authRequired(http.HandlerFunc(s.sourceStatus)))
	mux.Handle("GET /api/runtime-state-summary", s.authRequired(http.HandlerFunc(s.runtimeStateSummary)))
	mux.Handle("POST /api/auth/logout", s.authRequired(http.HandlerFunc(s.logout)))
	mux.Handle("POST /api/manual-control/preview", s.authRequired(http.HandlerFunc(s.manualControlPreview)))
	mux.Handle("POST /api/manual-control/route", s.authRequired(http.HandlerFunc(s.manualControlRoute)))

	routes := map[string]provider.Resource{
		"/api/shell-status":     provider.ResourceShellStatus,
		"/api/overview":         provider.ResourceOverview,
		"/api/positions":        provider.ResourcePositions,
		"/api/reconciliation":   provider.ResourceReconciliation,
		"/api/pipeline":         provider.ResourcePipeline,
		"/api/execution":        provider.ResourceExecution,
		"/api/risk":             provider.ResourceRisk,
		"/api/market-data":      provider.ResourceMarketData,
		"/api/infrastructure":   provider.ResourceInfrastructure,
		"/api/alerts-audit":     provider.ResourceAlertsAudit,
		"/api/live-vs-expected": provider.ResourceLiveVsExpected,
		"/api/manual-control":   provider.ResourceManualControl,
	}
	for path, resource := range routes {
		resourceName := resource
		mux.Handle("GET "+path, s.authRequired(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			s.serveResource(w, r, resourceName)
		})))
	}

	return s.middleware(mux)
}

func (s *Server) middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		s.metrics.requestStarted()
		tracked := &statusResponseWriter{ResponseWriter: w}
		tracked.Header().Set("X-Content-Type-Options", "nosniff")
		tracked.Header().Set("Cache-Control", "no-store")
		tracked.Header().Set("Referrer-Policy", "no-referrer")
		next.ServeHTTP(tracked, r)
		duration := time.Since(started)
		status := tracked.status
		if status == 0 {
			status = http.StatusOK
		}
		s.metrics.requestFinished(status, duration, r.URL.Path != "/api/stream")
		log := s.cfg.Logger.Info
		if status >= 500 || duration >= 2*time.Second {
			log = s.cfg.Logger.Warn
		}
		log("request", "method", r.Method, "path", r.URL.Path, "status", status, "duration_ms", duration.Milliseconds())
	})
}

func (s *Server) authRequired(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie("cd_session")
		if err != nil || cookie.Value == "" {
			s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
			return
		}
		value, ok := s.sessions.get(cookie.Value)
		if !ok {
			s.clearSessionCookie(w)
			s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "session expired or invalid"})
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), authContextKey{}, value)))
	})
}

func (s *Server) login(w http.ResponseWriter, r *http.Request) {
	key := clientKey(r)
	if !s.loginLimiter.allowed(key) {
		w.Header().Set("Retry-After", "60")
		s.writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "too many failed login attempts; try again later"})
		return
	}

	var input struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	decoder := json.NewDecoder(io.LimitReader(r.Body, 4096))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&input); err != nil {
		s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid login request"})
		return
	}
	input.Username = strings.TrimSpace(input.Username)
	user, ok := s.users[input.Username]
	if !ok || !secureEqual(input.Password, user.Password) {
		s.loginLimiter.failed(key)
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "invalid username or password"})
		return
	}

	token, value, err := s.sessions.create(user)
	if err != nil {
		s.cfg.Logger.Error("session creation failed", "error", err)
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "could not create session"})
		return
	}
	s.loginLimiter.succeeded(key)
	http.SetCookie(w, &http.Cookie{
		Name:     "cd_session",
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.CookieSecure,
		SameSite: http.SameSiteStrictMode,
		Expires:  value.ExpiresAt,
		MaxAge:   int(time.Until(value.ExpiresAt).Seconds()),
	})
	s.writeSession(w, value)
}

func (s *Server) me(w http.ResponseWriter, r *http.Request) {
	value, ok := sessionFromContext(r.Context())
	if !ok {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}
	s.writeSession(w, value)
}

func (s *Server) logout(w http.ResponseWriter, r *http.Request) {
	value, ok := sessionFromContext(r.Context())
	if !ok {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}
	if !secureEqual(r.Header.Get("X-CSRF-Token"), value.CSRFToken) {
		s.writeJSON(w, http.StatusForbidden, map[string]string{"error": "invalid CSRF token"})
		return
	}
	if cookie, err := r.Cookie("cd_session"); err == nil {
		s.sessions.delete(cookie.Value)
	}
	s.clearSessionCookie(w)
	s.writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) writeSession(w http.ResponseWriter, value session) {
	s.writeJSON(w, http.StatusOK, map[string]any{
		"authenticated": true,
		"user": map[string]any{
			"username": value.Username,
			"role":     value.Role,
		},
		"csrfToken": value.CSRFToken,
		"expiresAt": value.ExpiresAt.Format(time.RFC3339),
	})
}

func (s *Server) clearSessionCookie(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{
		Name:     "cd_session",
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.CookieSecure,
		SameSite: http.SameSiteStrictMode,
		Expires:  time.Unix(1, 0),
		MaxAge:   -1,
	})
}

func (s *Server) manualControlPreview(w http.ResponseWriter, r *http.Request) {
	value, ok := sessionFromContext(r.Context())
	if !ok {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}
	if value.Role != RoleOperator {
		s.writeJSON(w, http.StatusForbidden, map[string]string{"error": "OPERATOR role required"})
		return
	}
	if !secureEqual(r.Header.Get("X-CSRF-Token"), value.CSRFToken) {
		s.writeJSON(w, http.StatusForbidden, map[string]string{"error": "invalid CSRF token"})
		return
	}

	var input struct {
		Filename string `json:"filename"`
		CSV      string `json:"csv"`
	}
	decoder := json.NewDecoder(io.LimitReader(r.Body, 320*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&input); err != nil {
		s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid manual-control preview request"})
		return
	}
	if strings.TrimSpace(input.Filename) == "" {
		input.Filename = "manual-target.csv"
	}

	previewer, ok := s.provider.(interface {
		PreviewManualControl(context.Context, provider.ManualControlPreviewRequest) (provider.ManualControlData, error)
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "manual-control preview is not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()
	result, err := previewer.PreviewManualControl(ctx, provider.ManualControlPreviewRequest{
		Actor:    value.Username + " / " + string(value.Role),
		Filename: strings.TrimSpace(input.Filename),
		CSV:      input.CSV,
	})
	if err != nil {
		s.cfg.Logger.Error("manual-control preview failed", "actor", value.Username, "error", err)
		s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "manual-control preview unavailable"})
		return
	}
	// This is intentionally a non-mutating endpoint. A successful preview never
	// implies risk approval, routing, order creation or audit persistence.
	s.writeJSON(w, http.StatusOK, result)
}

type manualControlRouteResponse struct {
	ContractVersion          string   `json:"contractVersion"`
	Status                   string   `json:"status"`
	Submitted                bool     `json:"submitted"`
	RouteEnabled             bool     `json:"routeEnabled"`
	ConfirmationAccepted     bool     `json:"confirmationAccepted"`
	RequestHash              string   `json:"requestHash"`
	CorrelationID            string   `json:"correlationId"`
	Actor                    string   `json:"actor"`
	ReferenceTargetTimestamp string   `json:"referenceTargetTimestamp"`
	CheckedAt                string   `json:"checkedAt"`
	Blockers                 []string `json:"blockers"`
	AuditPersisted           bool     `json:"auditPersisted"`
	Note                     string   `json:"note"`
}

func (s *Server) manualControlRoute(w http.ResponseWriter, r *http.Request) {
	value, ok := sessionFromContext(r.Context())
	if !ok {
		s.writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "authentication required"})
		return
	}
	if value.Role != RoleOperator {
		s.writeJSON(w, http.StatusForbidden, map[string]string{"error": "OPERATOR role required"})
		return
	}
	if !secureEqual(r.Header.Get("X-CSRF-Token"), value.CSRFToken) {
		s.writeJSON(w, http.StatusForbidden, map[string]string{"error": "invalid CSRF token"})
		return
	}

	var input struct {
		Filename                 string `json:"filename"`
		CSV                      string `json:"csv"`
		RequestHash              string `json:"requestHash"`
		ReferenceTargetTimestamp string `json:"referenceTargetTimestamp"`
		Confirmation             string `json:"confirmation"`
	}
	decoder := json.NewDecoder(io.LimitReader(r.Body, 320*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&input); err != nil {
		s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid manual-control route request"})
		return
	}
	if input.Confirmation != provider.ManualControlConfirmationPhrase {
		s.writeJSON(w, http.StatusBadRequest, map[string]string{"error": "explicit manual-route confirmation is required"})
		return
	}
	if strings.TrimSpace(input.Filename) == "" {
		input.Filename = "manual-target.csv"
	}

	previewer, ok := s.provider.(interface {
		PreviewManualControl(context.Context, provider.ManualControlPreviewRequest) (provider.ManualControlData, error)
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "manual-control routing admission is not implemented by this provider"})
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	actor := value.Username + " / " + string(value.Role)
	preview, err := previewer.PreviewManualControl(ctx, provider.ManualControlPreviewRequest{
		Actor: actor, Filename: strings.TrimSpace(input.Filename), CSV: input.CSV,
	})
	if err != nil {
		// A confirmed OPERATOR intent is still audit-worthy when canonical read
		// dependencies are unavailable. Persist only bounded metadata/hash input,
		// never the CSV itself, and remain fail-closed.
		correlationID, correlationErr := newManualRouteCorrelationID()
		if correlationErr != nil {
			s.cfg.Logger.Error("manual-control route admission failed", "actor", value.Username, "error", err)
			s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "manual-control route admission unavailable"})
			return
		}
		checkedAt := time.Now().UTC().Format(time.RFC3339Nano)
		blockers := uniqueRouteBlockers([]string{
			"PREVIEW_UNAVAILABLE",
			"MANUAL_RISK_CONTRACT_UNAVAILABLE",
			"TRADING_CONTROL_SINK_UNCONFIGURED",
			"PRIVATE_AUTH_DEFERRED",
			"ORDER_LIFECYCLE_DEFERRED",
			"GLOBAL_TRADING_READINESS_FALSE",
		})
		result := manualControlRouteResponse{
			ContractVersion: provider.ManualControlContractVersion, Status: "BLOCKED", Submitted: false, RouteEnabled: false, ConfirmationAccepted: true,
			RequestHash: strings.TrimSpace(input.RequestHash), CorrelationID: correlationID, Actor: actor,
			ReferenceTargetTimestamp: strings.TrimSpace(input.ReferenceTargetTimestamp), CheckedAt: checkedAt, Blockers: blockers,
			Note: "Step 46 could not recompute the authoritative preview, so the confirmed OPERATOR intent was rejected fail-closed. No trading command or capital movement occurred.",
		}
		auditErr := s.manualAudit.Append(manualaudit.Event{
			Version: manualaudit.StoreVersion, EventID: correlationID, RecordedAt: checkedAt, Actor: actor,
			Action: "MANUAL_ROUTE_ADMISSION", Target: "portfolio", Result: "REJECTED",
			RequestHash: strings.TrimSpace(input.RequestHash), CorrelationID: correlationID,
			ReferenceTargetTimestamp: strings.TrimSpace(input.ReferenceTargetTimestamp), ContractVersion: provider.ManualControlContractVersion,
			Submitted: false, Blockers: blockers, Detail: "Manual routing admission rejected because authoritative preview was unavailable: " + err.Error(),
		})
		if auditErr == nil {
			result.AuditPersisted = true
		} else {
			result.Blockers = uniqueRouteBlockers(append(result.Blockers, "DURABLE_MANUAL_AUDIT_UNAVAILABLE"))
			result.Note += " Durable operator-intent audit persistence also failed."
		}
		s.cfg.Logger.Warn("manual-control route admission rejected because preview is unavailable", "actor", value.Username, "correlation_id", correlationID, "preview_error", err, "audit_persisted", result.AuditPersisted)
		s.writeJSON(w, http.StatusServiceUnavailable, result)
		return
	}

	blockers := append([]string(nil), preview.RouteBlockers...)
	if preview.ContractVersion != provider.ManualControlContractVersion || !preview.RoutingContractReady {
		blockers = append(blockers, "ROUTING_CONTRACT_NOT_READY")
	}
	if !preview.ValidationPassed {
		blockers = append(blockers, "PREVIEW_VALIDATION_FAILED")
	}
	if !preview.ExchangeConstraintsValidated {
		blockers = append(blockers, "EXCHANGE_CONSTRAINTS_NOT_VALIDATED")
	}
	if strings.TrimSpace(input.RequestHash) == "" || input.RequestHash != preview.RequestHash {
		blockers = append(blockers, "REQUEST_HASH_MISMATCH")
	}
	if strings.TrimSpace(input.ReferenceTargetTimestamp) == "" || input.ReferenceTargetTimestamp != preview.CurrentTargetTimestamp {
		blockers = append(blockers, "STALE_REFERENCE_TARGET")
	}
	// Step 46 deliberately has no trading-control sink and private venue/order
	// lifecycle remains deferred. Even a perfectly valid confirmed request can
	// only be admission-evaluated and durably audited; it cannot be submitted.
	blockers = append(blockers, "GLOBAL_TRADING_READINESS_FALSE")
	blockers = uniqueRouteBlockers(blockers)

	correlationID, err := newManualRouteCorrelationID()
	if err != nil {
		s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "could not create manual-route correlation id"})
		return
	}
	checkedAt := time.Now().UTC().Format(time.RFC3339Nano)
	result := manualControlRouteResponse{
		ContractVersion: provider.ManualControlContractVersion,
		Status:          "BLOCKED", Submitted: false, RouteEnabled: false, ConfirmationAccepted: true,
		RequestHash: preview.RequestHash, CorrelationID: correlationID, Actor: actor,
		ReferenceTargetTimestamp: preview.CurrentTargetTimestamp, CheckedAt: checkedAt, Blockers: blockers,
		AuditPersisted: false,
		Note:           "Step 46 admission evaluated the confirmed request and failed closed. No NATS publish, trading-control call, exchange API call, order creation, cancellation, signing or capital movement occurred.",
	}

	detail := "Manual routing admission blocked: " + strings.Join(blockers, ", ")
	auditEvent := manualaudit.Event{
		Version: manualaudit.StoreVersion, EventID: correlationID, RecordedAt: checkedAt,
		Actor: actor, Action: "MANUAL_ROUTE_ADMISSION", Target: "portfolio", Result: "REJECTED",
		RequestHash: preview.RequestHash, CorrelationID: correlationID,
		ReferenceTargetTimestamp: preview.CurrentTargetTimestamp, ContractVersion: provider.ManualControlContractVersion,
		Submitted: false, Blockers: blockers, Detail: detail,
	}
	if err := s.manualAudit.Append(auditEvent); err != nil {
		s.cfg.Logger.Error("manual-control audit persistence failed", "actor", value.Username, "correlation_id", correlationID, "error", err)
		result.Blockers = uniqueRouteBlockers(append(result.Blockers, "DURABLE_MANUAL_AUDIT_UNAVAILABLE"))
		result.Note += " Durable operator-intent audit persistence failed, so the request remains blocked."
		s.writeJSON(w, http.StatusServiceUnavailable, result)
		return
	}
	result.AuditPersisted = true
	s.cfg.Logger.Info("manual-control route admission blocked safely", "actor", value.Username, "correlation_id", correlationID, "request_hash", preview.RequestHash, "blocker_count", len(blockers))
	s.writeJSON(w, http.StatusOK, result)
}

func uniqueRouteBlockers(values []string) []string {
	seen := make(map[string]struct{}, len(values))
	out := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	sort.Strings(out)
	return out
}

func newManualRouteCorrelationID() (string, error) {
	var buf [12]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return "manual-route-" + hex.EncodeToString(buf[:]), nil
}

func (s *Server) providerStatus(w http.ResponseWriter, r *http.Request) {
	health := s.provider.Health(r.Context())
	s.writeJSON(w, http.StatusOK, health)
}

func (s *Server) venueFoundation(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		VenueFoundation(context.Context) provider.VenueFoundation
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "venue foundation is not implemented by this provider"})
		return
	}
	s.writeJSON(w, http.StatusOK, reader.VenueFoundation(r.Context()))
}

func (s *Server) venuePublic(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		VenuePublicStatus(context.Context) provider.VenuePublicStatus
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "public venue status is not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	result := reader.VenuePublicStatus(ctx)
	// Reachability failure is represented explicitly in the payload. The endpoint
	// itself remains readable so operators can see the fail-closed reason.
	s.writeJSON(w, http.StatusOK, result)
}

func (s *Server) venueSymbolMap(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		VenueSymbolMappingStatus(context.Context) provider.VenueSymbolMappingStatus
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "venue symbol mapping is not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	s.writeJSON(w, http.StatusOK, reader.VenueSymbolMappingStatus(ctx))
}

func (s *Server) venueRules(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		VenueTradingRulesStatus(context.Context) provider.VenueTradingRulesStatus
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "venue trading rules are not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	s.writeJSON(w, http.StatusOK, reader.VenueTradingRulesStatus(ctx))
}

func (s *Server) symbolRegistry(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		SymbolRegistryStatus(context.Context) provider.SymbolRegistryStatus
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "symbol registry is not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	s.writeJSON(w, http.StatusOK, reader.SymbolRegistryStatus(ctx))
}

func (s *Server) ledger(w http.ResponseWriter, r *http.Request) {
	reader, ok := s.provider.(interface {
		LedgerStatus(context.Context) provider.LedgerStatus
	})
	if !ok {
		s.writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "ledger foundation is not implemented by this provider"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	s.writeJSON(w, http.StatusOK, reader.LedgerStatus(ctx))
}

func (s *Server) sourceStatus(w http.ResponseWriter, r *http.Request) {
	// Source probes are intentionally available only on the real provider.
	if realProvider, ok := s.provider.(interface {
		SourceStatus(context.Context) probe.Status
	}); ok {
		ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
		defer cancel()
		s.writeJSON(w, http.StatusOK, map[string]any{
			"mode":    "real",
			"sources": realProvider.SourceStatus(ctx),
		})
		return
	}

	s.writeJSON(w, http.StatusOK, map[string]any{
		"mode":      "mock",
		"available": false,
		"detail":    "source probes are available only when DASHBOARD_DATA_PROVIDER=real",
	})
}

func (s *Server) runtimeStateSummary(w http.ResponseWriter, r *http.Request) {
	if realProvider, ok := s.provider.(interface {
		RuntimeStateSummary(context.Context) map[string]any
	}); ok {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		s.writeJSON(w, http.StatusOK, realProvider.RuntimeStateSummary(ctx))
		return
	}

	s.writeJSON(w, http.StatusOK, map[string]any{
		"mode":      "mock",
		"available": false,
		"detail":    "runtime-state summary is available only when DASHBOARD_DATA_PROVIDER=real",
	})
}

func (s *Server) integrationCatalog(w http.ResponseWriter, r *http.Request) {
	if err := catalog.Validate(); err != nil {
		s.cfg.Logger.Error("integration catalog invalid", "error", err)
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "integration catalog invalid"})
		return
	}
	s.writeJSON(w, http.StatusOK, map[string]any{
		"auditVersion": "step14-project-audit-2026-09-24",
		"mappings":     catalog.Mappings(),
	})
}

func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	health := s.provider.Health(r.Context())
	s.writeJSON(w, http.StatusOK, map[string]any{
		"status":        "ok",
		"service":       "dashboard-api",
		"version":       s.cfg.Version,
		"data_provider": health,
		"auth":          "session-cookie",
	})
}

func (s *Server) readiness(w http.ResponseWriter, r *http.Request) {
	health := s.provider.Health(r.Context())
	status := http.StatusOK
	label := "ready"
	if !health.Ready {
		status = http.StatusServiceUnavailable
		label = "not-ready"
	}
	s.writeJSON(w, status, map[string]any{
		"status":        label,
		"service":       "dashboard-api",
		"version":       s.cfg.Version,
		"data_provider": health,
	})
}

func (s *Server) serveResource(w http.ResponseWriter, r *http.Request, resource provider.Resource) {
	health := s.provider.Health(r.Context())
	w.Header().Set("X-Dashboard-Data-Provider", health.Name)

	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()
	body, err := s.provider.Read(ctx, resource)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) || errors.Is(ctx.Err(), context.DeadlineExceeded) {
			s.metrics.resourceTimeouts.Add(1)
		}
		s.cfg.Logger.Error("dashboard provider read failed", "resource", resource, "provider", health.Name, "error", err)
		s.writeJSON(w, http.StatusServiceUnavailable, map[string]string{
			"error":    "dashboard data unavailable",
			"resource": string(resource),
		})
		return
	}

	if !json.Valid(body) {
		s.cfg.Logger.Error("dashboard provider returned invalid JSON", "resource", resource, "provider", health.Name)
		s.writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "invalid provider response"})
		return
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

func (s *Server) writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
