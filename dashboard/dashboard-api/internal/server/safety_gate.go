package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"control-dashboard-api/internal/provider"
)

type safetyGateCheck struct {
	ID     string `json:"id"`
	Label  string `json:"label"`
	State  string `json:"state"`
	Detail string `json:"detail"`
}

type safetyGateResponse struct {
	Status              string            `json:"status"`
	SafeToProceed       bool              `json:"safeToProceed"`
	TradingReady        bool              `json:"tradingReady"`
	RuntimeReadiness    string            `json:"runtimeReadiness"`
	EndToEndProofStatus string            `json:"endToEndProofStatus"`
	EndToEndCompletion  string            `json:"endToEndCompletion"`
	CheckedAt           string            `json:"checkedAt"`
	Checks              []safetyGateCheck `json:"checks"`
	Note                string            `json:"note"`
}

func (s *Server) safetyGate(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.ResourceTimeout)
	defer cancel()

	checks := make([]safetyGateCheck, 0, 8)
	add := func(id, label, state, detail string) {
		checks = append(checks, safetyGateCheck{ID: id, Label: label, State: state, Detail: detail})
	}

	health := s.provider.Health(ctx)
	if strings.EqualFold(health.Mode, "real") {
		add("provider", "Real data provider", "PASS", "Dashboard is using the real read-only provider; no mock fallback is accepted by this gate.")
	} else {
		add("provider", "Real data provider", "BLOCKED", "Safety proof requires DASHBOARD_DATA_PROVIDER=real; current provider mode is "+health.Mode+".")
	}

	var shell struct {
		Readiness          string   `json:"readiness"`
		ReadinessDetail    string   `json:"readinessDetail"`
		TradingEnabled     bool     `json:"tradingEnabled"`
		TradingState       string   `json:"tradingState"`
		ExchangeConnected  bool     `json:"exchangeConnected"`
		Reconciliation     string   `json:"reconciliation"`
		DataState          string   `json:"dataState"`
		CriticalAlertCount int      `json:"criticalAlertCount"`
		Blockers           []string `json:"blockers"`
	}
	shellErr := s.readSafetyResource(ctx, provider.ResourceShellStatus, &shell)
	runtimeReadiness := "UNKNOWN"
	if shellErr != nil {
		add("runtime", "Runtime hard blockers", "BLOCKED", "Shell/readiness evidence is unavailable: "+shellErr.Error())
	} else {
		runtimeReadiness = shell.Readiness
		if len(shell.Blockers) > 0 {
			detail := "Verified runtime blocker(s): " + strings.Join(shell.Blockers, "; ")
			if strings.TrimSpace(shell.ReadinessDetail) != "" {
				detail += ". Shell detail: " + shell.ReadinessDetail
			}
			add("runtime", "Runtime hard blockers", "BLOCKED", detail)
		} else {
			switch shell.Readiness {
			case "READY":
				add("runtime", "Runtime hard blockers", "PASS", "The shell aggregate reports READY and exposes no verified blocker.")
			case "PAUSED":
				detail := "Shell readiness is PAUSED but no blocker list was exposed; fail closed until the reason is observable."
				if strings.TrimSpace(shell.ReadinessDetail) != "" {
					detail += " Shell detail: " + shell.ReadinessDetail
				}
				add("runtime", "Runtime hard blockers", "BLOCKED", detail)
			default:
				add("runtime", "Runtime hard blockers", "WARN", "The shell aggregate is "+shell.Readiness+" with no verified hard blocker. This is expected while exchange/service/clock readiness contracts are still incomplete; it is not permission to trade.")
			}
		}
		if shell.CriticalAlertCount > 0 {
			add("critical-alerts", "Critical operational alerts", "BLOCKED", fmt.Sprintf("%d critical alert(s) are active.", shell.CriticalAlertCount))
		} else {
			add("critical-alerts", "Critical operational alerts", "PASS", "No active critical alert is reported by the shell aggregate.")
		}
	}

	var pipeline struct {
		Proof *struct {
			Status     string `json:"status"`
			Completion string `json:"completion"`
		} `json:"proof"`
	}
	proofStatus := "UNKNOWN"
	proofCompletion := "UNKNOWN"
	if err := s.readSafetyResource(ctx, provider.ResourcePipeline, &pipeline); err != nil {
		add("e2e-proof", "Step 31 durable end-to-end proof", "BLOCKED", "Pipeline proof is unavailable: "+err.Error())
	} else if pipeline.Proof == nil {
		add("e2e-proof", "Step 31 durable end-to-end proof", "WARN", "No Step 31 proof is exposed by the current provider.")
	} else {
		proofStatus = pipeline.Proof.Status
		proofCompletion = pipeline.Proof.Completion
		switch pipeline.Proof.Status {
		case "BLOCKED":
			add("e2e-proof", "Step 31 durable end-to-end proof", "BLOCKED", "Latest durable cycle proof is BLOCKED ("+pipeline.Proof.Completion+").")
		case "ALIGNED":
			add("e2e-proof", "Step 31 durable end-to-end proof", "PASS", "Latest durable cycle has a complete aligned evidence chain ("+pipeline.Proof.Completion+").")
		default:
			add("e2e-proof", "Step 31 durable end-to-end proof", "WARN", "Latest durable cycle proof is "+pipeline.Proof.Status+" ("+pipeline.Proof.Completion+"). A quiet/no-action cycle is allowed; contradictions are not.")
		}
	}

	var manual struct {
		RouteEnabled bool `json:"routeEnabled"`
	}
	if err := s.readSafetyResource(ctx, provider.ResourceManualControl, &manual); err != nil {
		add("manual-routing", "Manual trading route", "WARN", "Manual-control read model is unavailable, so routeEnabled could not be re-verified: "+err.Error())
	} else if manual.RouteEnabled {
		add("manual-routing", "Manual trading route", "BLOCKED", "Manual routing is enabled before a verified business control contract exists.")
	} else {
		add("manual-routing", "Manual trading route", "PASS", "Manual Control remains preview-only; routeEnabled=false.")
	}

	if s.cfg.DemoAuthAllowed {
		add("demo-auth", "Demo credentials", "WARN", "Demo credentials are enabled. This is acceptable only for the local WSL development stack, never for a VPS/testnet deployment with secrets.")
	} else {
		add("demo-auth", "Demo credentials", "PASS", "Demo credentials are disabled.")
	}
	if s.cfg.CookieSecure {
		add("secure-cookie", "Secure session cookie", "PASS", "Session cookie is configured Secure.")
	} else {
		add("secure-cookie", "Secure session cookie", "WARN", "Secure cookie is disabled for local HTTP development. Production/testnet HTTPS deployment must enable it.")
	}

	// This is a server invariant, not a runtime guess: there is no submit/cancel/
	// pause/resume/kill route in the dashboard API. The only non-auth mutation-like
	// endpoint is the fail-closed manual preview.
	add("command-surface", "Dashboard command surface", "PASS", "No trading submit/cancel/pause/resume/kill endpoint is registered; the dashboard remains read-first and manual routing stays fail-closed.")

	blocked := false
	warned := false
	for _, check := range checks {
		if check.State == "BLOCKED" {
			blocked = true
		}
		if check.State == "WARN" {
			warned = true
		}
	}
	status := "PASS"
	if blocked {
		status = "BLOCKED"
	} else if warned {
		status = "WARN"
	}

	tradingReady := shellErr == nil && shell.Readiness == "READY" && shell.TradingEnabled && shell.ExchangeConnected && shell.Reconciliation == "CLEAN"
	response := safetyGateResponse{
		Status:              status,
		SafeToProceed:       !blocked,
		TradingReady:        tradingReady,
		RuntimeReadiness:    runtimeReadiness,
		EndToEndProofStatus: proofStatus,
		EndToEndCompletion:  proofCompletion,
		CheckedAt:           time.Now().UTC().Format(time.RFC3339),
		Checks:              checks,
		Note:                "Step 32 is a pre-testnet safety boundary, not authorization to trade. safeToProceed means no verified contradiction blocks continued testnet integration work. tradingReady remains independently fail-closed.",
	}
	s.writeJSON(w, http.StatusOK, response)
}

func (s *Server) readSafetyResource(ctx context.Context, resource provider.Resource, target any) error {
	raw, err := s.provider.Read(ctx, resource)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(raw, target); err != nil {
		return fmt.Errorf("decode %s: %w", resource, err)
	}
	return nil
}
