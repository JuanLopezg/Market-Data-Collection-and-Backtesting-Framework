package server

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"control-dashboard-api/internal/manualaudit"
	"control-dashboard-api/internal/provider"
)

func newMockProvider(t *testing.T) provider.Provider {
	t.Helper()
	p, err := provider.NewEmbeddedMock()
	if err != nil {
		t.Fatalf("NewEmbeddedMock() error = %v", err)
	}
	return p
}

func newTestServer(t *testing.T) *Server {
	t.Helper()
	api, err := New(Config{
		Logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
		Users: []AuthUser{
			{Username: "viewer", Password: "viewer-pass", Role: RoleViewer},
			{Username: "operator", Password: "operator-pass", Role: RoleOperator},
		},
		SessionTTL:     time.Hour,
		Provider:       newMockProvider(t),
		ManualAuditDir: t.TempDir(),
	})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	return api
}

func loginAs(t *testing.T, api *Server, username, password string) (*http.Cookie, string) {
	t.Helper()
	body, _ := json.Marshal(map[string]string{"username": username, "password": password})
	request := httptest.NewRequest(http.MethodPost, "/api/auth/login", bytes.NewReader(body))
	request.RemoteAddr = "127.0.0.1:12345"
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("login status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	result := response.Result()
	var cookie *http.Cookie
	for _, candidate := range result.Cookies() {
		if candidate.Name == "cd_session" {
			cookie = candidate
			break
		}
	}
	if cookie == nil {
		t.Fatal("login did not return cd_session cookie")
	}
	var payload struct {
		CSRFToken string `json:"csrfToken"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("login JSON invalid: %v", err)
	}
	if payload.CSRFToken == "" {
		t.Fatal("login did not return CSRF token")
	}
	return cookie, payload.CSRFToken
}

func TestHealthIsPublicAndIncludesProvider(t *testing.T) {
	api := newTestServer(t)
	request := httptest.NewRequest(http.MethodGet, "/api/health", nil)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusOK)
	}

	var body struct {
		Status       string          `json:"status"`
		DataProvider provider.Health `json:"data_provider"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid JSON: %v", err)
	}
	if body.Status != "ok" {
		t.Fatalf("status payload = %v", body.Status)
	}
	if body.DataProvider.Mode != "mock" || !body.DataProvider.Ready {
		t.Fatalf("unexpected provider health: %+v", body.DataProvider)
	}
	if body.DataProvider.ResourceCount != len(provider.AllResources) {
		t.Fatalf("resource count = %d, want %d", body.DataProvider.ResourceCount, len(provider.AllResources))
	}
}

func TestReadinessReflectsProviderState(t *testing.T) {
	mockAPI := newTestServer(t)
	request := httptest.NewRequest(http.MethodGet, "/api/readiness", nil)
	response := httptest.NewRecorder()
	mockAPI.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("mock readiness status = %d, want 200", response.Code)
	}

	realAPI, err := New(Config{
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		Users:    []AuthUser{{Username: "viewer", Password: "viewer-pass", Role: RoleViewer}},
		Provider: provider.NewReal(provider.RealConfig{}),
	})
	if err != nil {
		t.Fatalf("New(real) error = %v", err)
	}
	realRequest := httptest.NewRequest(http.MethodGet, "/api/readiness", nil)
	realResponse := httptest.NewRecorder()
	realAPI.Handler().ServeHTTP(realResponse, realRequest)
	if realResponse.Code != http.StatusServiceUnavailable {
		t.Fatalf("real skeleton readiness status = %d, want 503", realResponse.Code)
	}
}

func TestProviderStatusRequiresAuthenticationAndReportsMode(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/provider-status", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/provider-status", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", response.Code)
	}
	var health provider.Health
	if err := json.Unmarshal(response.Body.Bytes(), &health); err != nil {
		t.Fatalf("invalid provider status JSON: %v", err)
	}
	if health.Mode != "mock" || !health.Ready {
		t.Fatalf("unexpected provider status: %+v", health)
	}
}

func TestReadEndpointsRequireAuthentication(t *testing.T) {
	api := newTestServer(t)
	request := httptest.NewRequest(http.MethodGet, "/api/overview", nil)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusUnauthorized)
	}
}

func TestLoginMeAndReadEndpoints(t *testing.T) {
	api := newTestServer(t)
	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")

	meRequest := httptest.NewRequest(http.MethodGet, "/api/auth/me", nil)
	meRequest.AddCookie(cookie)
	meResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(meResponse, meRequest)
	if meResponse.Code != http.StatusOK {
		t.Fatalf("me status = %d, want 200", meResponse.Code)
	}
	var me struct {
		User struct {
			Username string `json:"username"`
			Role     Role   `json:"role"`
		} `json:"user"`
	}
	if err := json.Unmarshal(meResponse.Body.Bytes(), &me); err != nil {
		t.Fatalf("me invalid JSON: %v", err)
	}
	if me.User.Username != "viewer" || me.User.Role != RoleViewer {
		t.Fatalf("unexpected user payload: %+v", me.User)
	}

	paths := []string{
		"/api/shell-status",
		"/api/overview",
		"/api/positions",
		"/api/reconciliation",
		"/api/pipeline",
		"/api/execution",
		"/api/risk",
		"/api/market-data",
		"/api/infrastructure",
		"/api/alerts-audit",
		"/api/live-vs-expected",
		"/api/manual-control",
	}

	for _, path := range paths {
		t.Run(path, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodGet, path, nil)
			request.AddCookie(cookie)
			response := httptest.NewRecorder()
			api.Handler().ServeHTTP(response, request)

			if response.Code != http.StatusOK {
				t.Fatalf("status = %d, want %d; body=%s", response.Code, http.StatusOK, response.Body.String())
			}
			if contentType := response.Header().Get("Content-Type"); contentType != "application/json; charset=utf-8" {
				t.Fatalf("content type = %q", contentType)
			}
			if response.Header().Get("X-Dashboard-Data-Provider") != "embedded-mock-fixtures" {
				t.Fatalf("missing provider response header")
			}
			if !json.Valid(response.Body.Bytes()) {
				t.Fatalf("response is not valid JSON")
			}
		})
	}
}

func TestRealSkeletonFailsClosedForReads(t *testing.T) {
	api, err := New(Config{
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		Users:    []AuthUser{{Username: "viewer", Password: "viewer-pass", Role: RoleViewer}},
		Provider: provider.NewReal(provider.RealConfig{}),
	})
	if err != nil {
		t.Fatalf("New(real) error = %v", err)
	}
	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/overview", nil)
	request = request.WithContext(context.Background())
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503; body=%s", response.Code, response.Body.String())
	}
}

func TestInvalidLoginIsRejected(t *testing.T) {
	api := newTestServer(t)
	body := bytes.NewBufferString(`{"username":"viewer","password":"wrong"}`)
	request := httptest.NewRequest(http.MethodPost, "/api/auth/login", body)
	request.RemoteAddr = "127.0.0.2:1111"
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", response.Code)
	}
}

func TestLogoutRequiresCSRFAndInvalidatesSession(t *testing.T) {
	api := newTestServer(t)
	cookie, csrf := loginAs(t, api, "operator", "operator-pass")

	badRequest := httptest.NewRequest(http.MethodPost, "/api/auth/logout", nil)
	badRequest.AddCookie(cookie)
	badResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(badResponse, badRequest)
	if badResponse.Code != http.StatusForbidden {
		t.Fatalf("logout without CSRF status = %d, want 403", badResponse.Code)
	}

	request := httptest.NewRequest(http.MethodPost, "/api/auth/logout", nil)
	request.AddCookie(cookie)
	request.Header.Set("X-CSRF-Token", csrf)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("logout status = %d, want 200", response.Code)
	}

	meRequest := httptest.NewRequest(http.MethodGet, "/api/auth/me", nil)
	meRequest.AddCookie(cookie)
	meResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(meResponse, meRequest)
	if meResponse.Code != http.StatusUnauthorized {
		t.Fatalf("me after logout status = %d, want 401", meResponse.Code)
	}
}

func TestPostOverviewIsRejectedAfterAuthentication(t *testing.T) {
	api := newTestServer(t)
	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodPost, "/api/overview", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusMethodNotAllowed)
	}
}

func TestIntegrationCatalogRequiresAuthenticationAndCoversResources(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/integration-catalog", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/integration-catalog", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", response.Code, response.Body.String())
	}

	var body struct {
		AuditVersion string `json:"auditVersion"`
		Mappings     []struct {
			Resource provider.Resource `json:"resource"`
			Status   string            `json:"status"`
		} `json:"mappings"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid integration catalog JSON: %v", err)
	}
	if body.AuditVersion == "" {
		t.Fatal("missing audit version")
	}
	if len(body.Mappings) != len(provider.AllResources) {
		t.Fatalf("mapping count = %d, want %d", len(body.Mappings), len(provider.AllResources))
	}
}

func TestManualControlPreviewRequiresOperatorAndCSRF(t *testing.T) {
	api := newTestServer(t)
	viewerCookie, viewerCSRF := loginAs(t, api, "viewer", "viewer-pass")
	operatorCookie, operatorCSRF := loginAs(t, api, "operator", "operator-pass")
	body := bytes.NewBufferString(`{"filename":"targets.csv","csv":"asset,weight_pct\nBTC,50\nCASH,50"}`)

	viewerReq := httptest.NewRequest(http.MethodPost, "/api/manual-control/preview", body)
	viewerReq.AddCookie(viewerCookie)
	viewerReq.Header.Set("X-CSRF-Token", viewerCSRF)
	viewerRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(viewerRes, viewerReq)
	if viewerRes.Code != http.StatusForbidden {
		t.Fatalf("viewer preview status = %d, want 403", viewerRes.Code)
	}

	missingCSRFReq := httptest.NewRequest(http.MethodPost, "/api/manual-control/preview", bytes.NewBufferString(`{"filename":"targets.csv","csv":"asset,weight_pct\nBTC,50\nCASH,50"}`))
	missingCSRFReq.AddCookie(operatorCookie)
	missingCSRFRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(missingCSRFRes, missingCSRFReq)
	if missingCSRFRes.Code != http.StatusForbidden {
		t.Fatalf("operator preview without CSRF status = %d, want 403", missingCSRFRes.Code)
	}

	validReq := httptest.NewRequest(http.MethodPost, "/api/manual-control/preview", bytes.NewBufferString(`{"filename":"targets.csv","csv":"asset,weight_pct\nBTC,50\nCASH,50"}`))
	validReq.AddCookie(operatorCookie)
	validReq.Header.Set("X-CSRF-Token", operatorCSRF)
	validRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(validRes, validReq)
	if validRes.Code != http.StatusOK {
		t.Fatalf("operator preview status = %d, want 200; body=%s", validRes.Code, validRes.Body.String())
	}
	var payload provider.ManualControlData
	if err := json.Unmarshal(validRes.Body.Bytes(), &payload); err != nil {
		t.Fatalf("preview JSON invalid: %v", err)
	}
	if payload.RouteEnabled || payload.RiskCheckAvailable {
		t.Fatal("preview must remain non-executing")
	}
	if payload.AuditActorLabel != "operator / OPERATOR" {
		t.Fatalf("actor label = %q", payload.AuditActorLabel)
	}
}

func TestManualControlRouteRequiresOperatorCSRFConfirmationAndPersistsBlockedIntent(t *testing.T) {
	api := newTestServer(t)
	viewerCookie, viewerCSRF := loginAs(t, api, "viewer", "viewer-pass")
	operatorCookie, operatorCSRF := loginAs(t, api, "operator", "operator-pass")

	unauth := httptest.NewRequest(http.MethodPost, "/api/manual-control/route", bytes.NewBufferString(`{}`))
	unauthRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthRes, unauth)
	if unauthRes.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated route status = %d, want 401", unauthRes.Code)
	}

	viewerReq := httptest.NewRequest(http.MethodPost, "/api/manual-control/route", bytes.NewBufferString(`{"confirmation":"CONFIRM_MANUAL_ROUTE"}`))
	viewerReq.AddCookie(viewerCookie)
	viewerReq.Header.Set("X-CSRF-Token", viewerCSRF)
	viewerRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(viewerRes, viewerReq)
	if viewerRes.Code != http.StatusForbidden {
		t.Fatalf("viewer route status = %d, want 403", viewerRes.Code)
	}

	missingCSRF := httptest.NewRequest(http.MethodPost, "/api/manual-control/route", bytes.NewBufferString(`{"confirmation":"CONFIRM_MANUAL_ROUTE"}`))
	missingCSRF.AddCookie(operatorCookie)
	missingCSRFRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(missingCSRFRes, missingCSRF)
	if missingCSRFRes.Code != http.StatusForbidden {
		t.Fatalf("operator route without CSRF status = %d, want 403", missingCSRFRes.Code)
	}

	badConfirmation := httptest.NewRequest(http.MethodPost, "/api/manual-control/route", bytes.NewBufferString(`{"confirmation":"NO"}`))
	badConfirmation.AddCookie(operatorCookie)
	badConfirmation.Header.Set("X-CSRF-Token", operatorCSRF)
	badConfirmationRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(badConfirmationRes, badConfirmation)
	if badConfirmationRes.Code != http.StatusBadRequest {
		t.Fatalf("bad confirmation status = %d, want 400", badConfirmationRes.Code)
	}

	body := `{"filename":"targets.csv","csv":"asset,weight_pct\\nBTC,50\\nCASH,50","requestHash":"sha256:tampered","referenceTargetTimestamp":"stale","confirmation":"CONFIRM_MANUAL_ROUTE"}`
	validReq := httptest.NewRequest(http.MethodPost, "/api/manual-control/route", bytes.NewBufferString(body))
	validReq.AddCookie(operatorCookie)
	validReq.Header.Set("X-CSRF-Token", operatorCSRF)
	validRes := httptest.NewRecorder()
	api.Handler().ServeHTTP(validRes, validReq)
	if validRes.Code != http.StatusOK {
		t.Fatalf("route admission status = %d, want 200; body=%s", validRes.Code, validRes.Body.String())
	}
	var result manualControlRouteResponse
	if err := json.Unmarshal(validRes.Body.Bytes(), &result); err != nil {
		t.Fatalf("route response JSON invalid: %v", err)
	}
	if result.Status != "BLOCKED" || result.Submitted || result.RouteEnabled || !result.ConfirmationAccepted || !result.AuditPersisted {
		t.Fatalf("route admission must be durably audited and fail closed: %+v", result)
	}
	if result.CorrelationID == "" {
		t.Fatal("route admission missing correlation id")
	}
	wantBlockers := map[string]bool{"ROUTING_CONTRACT_NOT_READY": false, "EXCHANGE_CONSTRAINTS_NOT_VALIDATED": false, "REQUEST_HASH_MISMATCH": false, "STALE_REFERENCE_TARGET": false, "GLOBAL_TRADING_READINESS_FALSE": false}
	for _, blocker := range result.Blockers {
		if _, ok := wantBlockers[blocker]; ok {
			wantBlockers[blocker] = true
		}
	}
	for blocker, found := range wantBlockers {
		if !found {
			t.Fatalf("route admission missing blocker %q: %+v", blocker, result.Blockers)
		}
	}

	snapshot := manualaudit.ReadRecent(api.cfg.ManualAuditDir, 10)
	if !snapshot.Available || len(snapshot.Events) != 1 {
		t.Fatalf("manual audit snapshot = %+v", snapshot)
	}
	event := snapshot.Events[0]
	if event.CorrelationID != result.CorrelationID || event.Actor != "operator / OPERATOR" || event.Submitted || event.Action != "MANUAL_ROUTE_ADMISSION" {
		t.Fatalf("unexpected manual audit event: %+v", event)
	}
}

func TestSafetyGateRequiresAuthenticationAndBlocksMockMode(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/safety-gate", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated safety gate status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/safety-gate", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("safety gate status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var payload safetyGateResponse
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("safety gate JSON invalid: %v", err)
	}
	if payload.Status != "BLOCKED" || payload.SafeToProceed {
		t.Fatalf("mock safety gate must fail closed: %+v", payload)
	}
}

func TestVenueFoundationRequiresAuthenticationAndIsReadOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/venue-foundation", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/venue-foundation", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body provider.VenueFoundation
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid venue foundation JSON: %v", err)
	}
	if body.OrderRouting != "DISABLED" || body.PrivateAuth != "DISABLED" || !body.ReadOnly {
		t.Fatalf("unexpected mock venue boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/venue-foundation", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST venue foundation status = %d, want 405", postResponse.Code)
	}
}

func TestVenuePublicRequiresAuthenticationAndIsReadOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/venue-public", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated venue-public status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/venue-public", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("venue-public status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body provider.VenuePublicStatus
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid venue-public JSON: %v", err)
	}
	if body.Status != "MOCK_ONLY" || body.Connected || body.OrderRouting != "DISABLED" || body.PrivateAuth != "DISABLED" || !body.ReadOnly {
		t.Fatalf("unexpected mock public venue boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/venue-public", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST venue-public status = %d, want 405", postResponse.Code)
	}
}

func TestVenueRulesRequiresAuthenticationAndIsReadOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/venue-rules", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated venue-rules status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/venue-rules", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("venue-rules status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body provider.VenueTradingRulesStatus
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid venue-rules JSON: %v", err)
	}
	if body.Status != "MOCK_ONLY" || body.Validated || body.OrderRouting != "DISABLED" || body.PrivateAuth != "DISABLED" || !body.ReadOnly {
		t.Fatalf("unexpected mock venue-rules boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/venue-rules", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST venue-rules status = %d, want 405", postResponse.Code)
	}
}

func TestSymbolRegistryRequiresAuthenticationAndIsReadOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/symbol-registry", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated symbol-registry status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/symbol-registry", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("symbol-registry status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body provider.SymbolRegistryStatus
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid symbol-registry JSON: %v", err)
	}
	if body.Status != "MOCK_ONLY" || body.Validated || body.OrderRouting != "DISABLED" || body.PrivateAuth != "DEFERRED" || !body.ReadOnly {
		t.Fatalf("unexpected mock symbol-registry boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/symbol-registry", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST symbol-registry status = %d, want 405", postResponse.Code)
	}
}

func TestLedgerRequiresAuthenticationAndIsReadOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/ledger", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated ledger status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/ledger", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("ledger status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body provider.LedgerStatus
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid ledger JSON: %v", err)
	}
	if body.Status != "MOCK_ONLY" || body.Validated || body.FoundationReady || body.OrderRouting != "DISABLED" || body.PrivateAuth != "DEFERRED" || !body.ReadOnly {
		t.Fatalf("unexpected mock ledger boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/ledger", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST ledger status = %d, want 405", postResponse.Code)
	}
}

func TestGlobalReadinessRequiresAuthenticationAndIsGetOnly(t *testing.T) {
	api := newTestServer(t)

	unauthenticated := httptest.NewRequest(http.MethodGet, "/api/global-readiness", nil)
	unauthenticatedResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(unauthenticatedResponse, unauthenticated)
	if unauthenticatedResponse.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated global-readiness status = %d, want 401", unauthenticatedResponse.Code)
	}

	cookie, _ := loginAs(t, api, "viewer", "viewer-pass")
	request := httptest.NewRequest(http.MethodGet, "/api/global-readiness", nil)
	request.AddCookie(cookie)
	response := httptest.NewRecorder()
	api.Handler().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("global-readiness status = %d, want 200; body=%s", response.Code, response.Body.String())
	}
	var body globalReadinessResponse
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("invalid global-readiness JSON: %v", err)
	}
	if body.ContractVersion != globalReadinessContractVersion || body.TradingReady || body.LiveReady {
		t.Fatalf("unexpected mock readiness boundary: %+v", body)
	}

	post := httptest.NewRequest(http.MethodPost, "/api/global-readiness", nil)
	post.AddCookie(cookie)
	postResponse := httptest.NewRecorder()
	api.Handler().ServeHTTP(postResponse, post)
	if postResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST global-readiness status = %d, want 405", postResponse.Code)
	}
}
