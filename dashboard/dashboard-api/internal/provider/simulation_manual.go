package provider

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Manual Control for the local replay simulation.
//
// The operator uploads a target portfolio CSV (asset,weight_pct). The API validates it
// against the portfolio-risk limits the replay publishes and previews the change against
// the current account; on confirmation it queues a request file in
// <simulation dir>/requests. The replay executes the newest request at the next price it
// has (the open for overnight requests, the close for daytime ones) through the normal
// planner -> MOCK venue path; the end-of-day decision then brings the account back to
// the strategy targets. The browser never talks to the venue.

const (
	manualMaxRows         = 100
	manualWeightEpsilon   = 1e-6
	manualRequestsSubdir  = "requests"
	manualProcessedSubdir = "processed"
	manualStatusLimit     = 10
)

type SimulationManualRouteRequest struct {
	Actor                    string
	Filename                 string
	CSV                      string
	RequestHash              string
	ReferenceTargetTimestamp string
}

type SimulationManualRouteResult struct {
	ContractVersion          string   `json:"contractVersion"`
	Status                   string   `json:"status"` // QUEUED or BLOCKED
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

// Portfolio-risk limits in percent of equity. States written before the replay
// published them fall back to an unlevered 100%.
type manualLimits struct {
	grossPct float64
	assetPct float64
}

func simulationManualLimits(s simulationSnapshot) manualLimits {
	limits := manualLimits{grossPct: 100, assetPct: 100}
	if s.Manual.MaxGrossLeverage > 0 {
		limits.grossPct = s.Manual.MaxGrossLeverage * 100
	}
	if s.Manual.MaxAssetWeight > 0 {
		limits.assetPct = s.Manual.MaxAssetWeight * 100
	}
	return limits
}

func (p *Simulation) PreviewManualControl(_ context.Context, req ManualControlPreviewRequest) (ManualControlData, error) {
	s, err := p.load()
	if err != nil {
		return ManualControlData{}, err
	}
	return p.manualPreview(s, req), nil
}

// RouteManualControl re-validates the CSV, checks it is the exact previewed request and
// queues it for the replay. The replay itself re-checks routes, prices and risk limits.
func (p *Simulation) RouteManualControl(_ context.Context, req SimulationManualRouteRequest) (SimulationManualRouteResult, error) {
	s, err := p.load()
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	preview := p.manualPreview(s, ManualControlPreviewRequest{Actor: req.Actor, Filename: req.Filename, CSV: req.CSV})
	result := SimulationManualRouteResult{
		ContractVersion: simulationManualContractVersion, Status: "BLOCKED", RouteEnabled: preview.RouteEnabled,
		ConfirmationAccepted: true, RequestHash: preview.RequestHash, Actor: req.Actor,
		ReferenceTargetTimestamp: strconv.FormatUint(s.BusinessTimestamp, 10),
		CheckedAt:                time.Now().UTC().Format(time.RFC3339Nano), Blockers: append([]string{}, preview.RouteBlockers...),
	}
	if req.RequestHash == "" || req.RequestHash != preview.RequestHash {
		result.Blockers = append(result.Blockers, "REQUEST_HASH_MISMATCH")
	}
	result.Blockers = uniqueStrings(result.Blockers)
	if !preview.ValidationPassed || !preview.RouteEnabled || len(result.Blockers) > 0 {
		result.Note = "Request rejected before reaching the replay."
		return result, nil
	}

	targets, _, _ := parseManualTargetCSV(req.CSV, routableSet(s), simulationManualLimits(s))
	id, err := p.queueManualRequest(req.Actor, preview.RequestHash, targets)
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	result.Status = "QUEUED"
	result.CorrelationID = id
	result.AuditPersisted = true
	result.Note = "Queued. The replay executes it at its next price; the end-of-day decision then returns to the strategy targets."
	return result, nil
}

func manualRouteEnabled(s simulationSnapshot) bool {
	return s.Manual.Ready && s.Reconciliation.State == "CLEAN" && s.RouteSafe
}

func routableSet(s simulationSnapshot) map[string]bool {
	out := make(map[string]bool, len(s.RoutableAssets))
	for _, asset := range s.RoutableAssets {
		out[asset] = true
	}
	return out
}

// manualBase is the page without an uploaded file: limits, blockers and request progress.
func (p *Simulation) manualBase(s simulationSnapshot, actor string) (ManualControlData, error) {
	blockers := []string{}
	if !s.Manual.Ready {
		blocker := s.Manual.Blocker
		if blocker == "" {
			blocker = "REPLAY_NOT_RUNNING"
		}
		blockers = append(blockers, blocker)
	}
	if s.Reconciliation.State != "CLEAN" {
		blockers = append(blockers, "RECONCILIATION_NOT_CLEAN")
	}
	if !s.RouteSafe {
		blockers = append(blockers, "ROUTE_SAFETY_FALSE")
	}
	limits := simulationManualLimits(s)
	return ManualControlData{
		ContractVersion: simulationManualContractVersion, RoutingContractReady: s.Manual.Ready,
		RoutingContractMode: "REPLAY_MANUAL_REBALANCE",
		ExecutionBoundary:   "OPERATOR -> dashboard-api -> request file -> replay -> planner -> MOCK venue",
		TradingControlSink:  "REPLAY_REQUEST_FILES", PrivateAuth: "DISABLED", OrderLifecycle: "CANONICAL_MOCK",
		ConfirmationRequired: true, ConfirmationPhrase: ManualControlConfirmationPhrase,
		HumanAuditAvailable: true, RouteBlockers: uniqueStrings(blockers), RecentRouteAudits: []ManualRouteAuditRow{},
		Mode: "REPLAY", Exchange: "MOCK", SchemaLabel: "asset,weight_pct", MaxUploadSizeLabel: "256 KB",
		ExampleCSV: manualExampleCSV(s), PreviewRows: []ManualPreviewRow{}, ValidationIssues: []ManualValidationIssue{},
		OrderPreview: []ManualOrderPreviewRow{}, EstimatedFeesLabel: "—", EstimatedTurnoverLabel: "—",
		AuditActorLabel: actor, BackendAuthoritative: true, RiskCheckAvailable: s.Manual.Ready, RouteEnabled: manualRouteEnabled(s),
		SourceMode: "MOCK", CurrentTargetTimestamp: strconv.FormatUint(s.BusinessTimestamp, 10), PreviewKind: "TARGET_WEIGHTS",
		SafetyNote:        "Replay simulation only: executes at the next replay price; the end-of-day decision returns to the strategy targets.",
		AcceptingRequests: s.Manual.Ready, MaxGrossPct: limits.grossPct, MaxAssetPct: limits.assetPct,
		Requests: p.manualRequestStatuses(s),
	}, nil
}

// manualPreview validates the CSV and compares it with the current account weights.
// Held assets missing from the CSV are shown as closed, because the replay closes them.
func (p *Simulation) manualPreview(s simulationSnapshot, req ManualControlPreviewRequest) ManualControlData {
	result, _ := p.manualBase(s, req.Actor)
	targets, cash, issues := parseManualTargetCSV(req.CSV, routableSet(s), simulationManualLimits(s))
	result.ValidationIssues = issues
	result.ValidationPassed = !hasManualError(issues)
	result.ExchangeConstraintsValidated = result.ValidationPassed
	if !result.ValidationPassed {
		result.RouteEnabled = false
		result.RouteBlockers = uniqueStrings(append(result.RouteBlockers, "PREVIEW_VALIDATION_FAILED"))
		return result
	}

	current := currentWeights(s)
	assets := make([]string, 0, len(targets)+len(current))
	seen := map[string]bool{}
	for asset := range targets {
		assets, seen[asset] = append(assets, asset), true
	}
	for asset, weight := range current {
		if asset != "CASH" && !seen[asset] && math.Abs(weight) > manualWeightEpsilon {
			assets, seen[asset] = append(assets, asset), true
		}
	}
	sort.Strings(assets)

	turnover := 0.0
	for _, asset := range assets {
		requested := targets[asset]
		delta := requested - current[asset]
		turnover += math.Abs(delta)
		action := "HOLD"
		switch {
		case requested == 0 && math.Abs(current[asset]) > manualWeightEpsilon:
			action = "CLOSE"
		case delta > manualWeightEpsilon:
			action = "BUY"
		case delta < -manualWeightEpsilon:
			action = "SELL"
		}
		result.PreviewRows = append(result.PreviewRows, ManualPreviewRow{
			Asset: asset, CurrentWeightPct: current[asset], RequestedWeightPct: requested, ApprovedWeightPct: requested,
			DeltaPct: delta, EstimatedNotionalLabel: formatSignedUSD(delta / 100 * s.Account.Equity), Action: action,
		})
	}
	result.PreviewRows = append(result.PreviewRows, ManualPreviewRow{
		Asset: "CASH", CurrentWeightPct: current["CASH"], RequestedWeightPct: cash, ApprovedWeightPct: cash,
		DeltaPct: cash - current["CASH"], EstimatedNotionalLabel: "—",
	})
	result.EstimatedTurnoverLabel = fmt.Sprintf("%.2f%%", turnover/2)

	digest := sha256.Sum256([]byte(canonicalManualTargets(targets, cash)))
	result.RequestHash = "sha256:" + hex.EncodeToString(digest[:])
	return result
}

func hasManualError(issues []ManualValidationIssue) bool {
	for _, issue := range issues {
		if issue.Severity == "ERROR" {
			return true
		}
	}
	return false
}

// parseManualTargetCSV reads asset,weight_pct rows (weights in percent of equity).
// Accepts a UTF-8 BOM and the Excel ";" + decimal-comma format. CASH is optional:
// when omitted, whatever is not allocated stays in cash (negative = borrowed, only up
// to the risk gross limit). Limits are rejected, never clipped.
func parseManualTargetCSV(raw string, routable map[string]bool, limits manualLimits) (map[string]float64, float64, []ManualValidationIssue) {
	targets := map[string]float64{}
	issues := []ManualValidationIssue{}
	fail := func(field, message string) {
		issues = append(issues, ManualValidationIssue{Severity: "ERROR", Field: field, Message: message})
	}

	if len(raw) > manualControlMaxCSVBytes {
		fail("file", "CSV exceeds the 256 KB limit.")
		return targets, 0, issues
	}
	text := strings.TrimPrefix(strings.ReplaceAll(raw, "\r\n", "\n"), string(rune(0xFEFF)))
	lines := []string{}
	for _, line := range strings.Split(text, "\n") {
		if strings.TrimSpace(line) != "" {
			lines = append(lines, line)
		}
	}
	if len(lines) == 0 {
		fail("file", "CSV is empty.")
		return targets, 0, issues
	}

	separator, decimalComma := ",", false
	if strings.Contains(lines[0], ";") {
		separator, decimalComma = ";", true
	}
	header := strings.Split(lines[0], separator)
	if len(header) != 2 || strings.ToLower(strings.TrimSpace(header[0])) != "asset" || strings.ToLower(strings.TrimSpace(header[1])) != "weight_pct" {
		fail("header", "The first line must be: asset,weight_pct")
		return targets, 0, issues
	}
	if len(lines)-1 > manualMaxRows {
		fail("rows", fmt.Sprintf("At most %d rows.", manualMaxRows))
		return targets, 0, issues
	}

	cash, cashGiven, total := 0.0, false, 0.0
	for i, line := range lines[1:] {
		row := fmt.Sprintf("row %d", i+2)
		fields := strings.Split(line, separator)
		if len(fields) != 2 {
			fail(row, "Expected two values: asset and weight_pct.")
			continue
		}
		asset := strings.ToUpper(strings.TrimSpace(fields[0]))
		value := strings.TrimSuffix(strings.TrimSpace(fields[1]), "%")
		if decimalComma {
			value = strings.ReplaceAll(value, ",", ".")
		}
		weight, err := strconv.ParseFloat(strings.TrimSpace(value), 64)
		if asset == "" {
			fail(row, "Asset is empty.")
			continue
		}
		if err != nil || math.IsNaN(weight) || math.IsInf(weight, 0) || weight < 0 {
			fail(asset, "weight_pct must be a non-negative number.")
			continue
		}
		if asset == "CASH" {
			if cashGiven {
				fail(asset, "CASH appears twice.")
				continue
			}
			cash, cashGiven = weight, true
			continue
		}
		if _, dup := targets[asset]; dup {
			fail(asset, "Asset appears twice.")
			continue
		}
		if weight > limits.assetPct+manualWeightEpsilon {
			fail(asset, fmt.Sprintf("%.2f%% is above the risk limit of %.0f%% per asset.", weight, limits.assetPct))
			continue
		}
		if !routable[asset] {
			if routable[asset+"USDT"] {
				fail(asset, fmt.Sprintf("Unknown asset. Did you mean %sUSDT?", asset))
			} else {
				fail(asset, "Not tradable in the simulation right now (no price today or no MOCK route).")
			}
			continue
		}
		targets[asset] = weight
		total += weight
	}

	switch {
	case total > limits.grossPct+manualWeightEpsilon:
		fail("weights", fmt.Sprintf("Asset weights sum to %.2f%%; the risk limit is %.0f%% of equity (%.2fx).", total, limits.grossPct, limits.grossPct/100))
	case cashGiven && math.Abs(total+cash-100) > manualWeightEpsilon:
		fail("weights", fmt.Sprintf("Weights including CASH sum to %.2f%%; they must sum to 100%%.", total+cash))
	case !cashGiven:
		cash = 100 - total
	}
	if !hasManualError(issues) {
		issues = append(issues, ManualValidationIssue{Severity: "INFO", Field: "weights",
			Message: fmt.Sprintf("%d assets, %.2f%% invested (%.2fx), %.2f%% cash.", len(targets), total, total/100, cash)})
	}
	return targets, cash, issues
}

func canonicalManualTargets(targets map[string]float64, cash float64) string {
	keys := make([]string, 0, len(targets))
	for asset := range targets {
		keys = append(keys, asset)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, asset := range keys {
		fmt.Fprintf(&b, "%s=%.12f\n", asset, targets[asset])
	}
	fmt.Fprintf(&b, "CASH=%.12f\n", cash)
	return b.String()
}

func manualExampleCSV(s simulationSnapshot) string {
	routable := routableSet(s)
	picks := []string{}
	for _, asset := range []string{"BTCUSDT", "ETHUSDT"} {
		if routable[asset] {
			picks = append(picks, asset)
		}
	}
	if len(picks) < 2 {
		sorted := append([]string(nil), s.RoutableAssets...)
		sort.Strings(sorted)
		for _, asset := range sorted {
			if len(picks) == 2 {
				break
			}
			if !routable[asset] || (len(picks) == 1 && picks[0] == asset) {
				continue
			}
			picks = append(picks, asset)
		}
	}
	var b strings.Builder
	b.WriteString("asset,weight_pct\n")
	weights := []int{40, 30}
	used := 0
	for i, asset := range picks {
		fmt.Fprintf(&b, "%s,%d\n", asset, weights[i])
		used += weights[i]
	}
	fmt.Fprintf(&b, "CASH,%d", 100-used)
	return b.String()
}

// queueManualRequest writes REPLAY_MANUAL_V1 atomically (tmp + rename). The name starts
// with a nanosecond timestamp so the replay keeps arrival order.
func (p *Simulation) queueManualRequest(actor, hash string, targets map[string]float64) (string, error) {
	id, err := manualRequestID()
	if err != nil {
		return "", err
	}
	dir := filepath.Join(p.cfg.Dir, manualRequestsSubdir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	temp, err := os.CreateTemp(dir, ".manual-*.tmp")
	if err != nil {
		return "", err
	}
	writer := bufio.NewWriter(temp)
	fmt.Fprintln(writer, "REPLAY_MANUAL_V1")
	fmt.Fprintf(writer, "request_id=%s\n", id)
	fmt.Fprintf(writer, "actor=%s\n", sanitizeLine(actor))
	fmt.Fprintf(writer, "request_hash=%s\n", hash)
	fmt.Fprintln(writer, "action=SET")
	assets := make([]string, 0, len(targets))
	for asset := range targets {
		assets = append(assets, asset)
	}
	sort.Strings(assets)
	for _, asset := range assets {
		fmt.Fprintf(writer, "target=%s,%.12f\n", asset, targets[asset]/100)
	}
	cleanup := func(err error) (string, error) {
		temp.Close()
		os.Remove(temp.Name())
		return "", err
	}
	if err := writer.Flush(); err != nil {
		return cleanup(err)
	}
	if err := temp.Sync(); err != nil {
		return cleanup(err)
	}
	if err := temp.Close(); err != nil {
		os.Remove(temp.Name())
		return "", err
	}
	final := filepath.Join(dir, fmt.Sprintf("%019d-%s.request", time.Now().UnixNano(), id))
	if err := os.Rename(temp.Name(), final); err != nil {
		os.Remove(temp.Name())
		return "", err
	}
	return id, nil
}

// manualRequestStatuses follows each recent request to the exchange: queued file ->
// executed by the replay -> its orders at the venue -> filled -> strategies re-synced
// at the end-of-day decision. Newest first.
func (p *Simulation) manualRequestStatuses(s simulationSnapshot) []ManualRequestStatus {
	statuses := []ManualRequestStatus{}
	type file struct{ name, dir string }
	files := []file{}
	for _, dir := range []string{manualRequestsSubdir, manualProcessedSubdir} {
		entries, err := os.ReadDir(filepath.Join(p.cfg.Dir, dir))
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".request") {
				files = append(files, file{entry.Name(), dir})
			}
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].name > files[j].name }) // queue time prefix
	if len(files) > manualStatusLimit {
		files = files[:manualStatusLimit]
	}

	orders := make(map[string]simulationOrder, len(s.Orders))
	for _, order := range s.Orders {
		orders[order.OrderID] = order
	}
	for _, f := range files {
		fields, _ := readManualRecord(filepath.Join(p.cfg.Dir, f.dir, f.name))
		status := ManualRequestStatus{ID: fields["request_id"], Stage: "QUEUED", Detail: "Waiting for the next replay price"}
		if f.dir == manualProcessedSubdir {
			switch fields["result"] {
			case "EXECUTED":
				status.Stage, status.Detail = "EXECUTED", fields["detail"]
			default:
				status.Stage, status.Detail = "FAILED", fields["detail"]
			}
		}
		if status.Stage == "EXECUTED" {
			first, last, ok := parseOrderRange(fields["orders"])
			if !ok {
				status.Stage = "RESYNCED" // nothing to trade: already at the target
			}
			for id := first; ok && id <= last; id++ {
				order, seen := orders[strconv.FormatUint(id, 10)]
				if !seen {
					continue
				}
				status.Orders++
				if order.Status == "FILLED" {
					status.Filled++
				}
			}
			if status.Orders > 0 {
				status.Stage = "AT_EXCHANGE"
				if status.Filled == status.Orders {
					status.Stage = "FILLED"
					// The end-of-day decision after this request has run.
					if !s.Manual.ResyncPending || s.Manual.LastCorrelationID != status.ID {
						status.Stage = "RESYNCED"
					}
				}
			}
		}
		statuses = append(statuses, status)
	}
	return statuses
}

func parseOrderRange(value string) (uint64, uint64, bool) {
	first, last, found := strings.Cut(value, "-")
	if !found {
		return 0, 0, false
	}
	a, errA := strconv.ParseUint(first, 10, 64)
	b, errB := strconv.ParseUint(last, 10, 64)
	return a, b, errA == nil && errB == nil && a <= b
}

func manualRequestID() (string, error) {
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return "manual-" + hex.EncodeToString(raw[:]), nil
}

func sanitizeLine(v string) string {
	return strings.ReplaceAll(strings.ReplaceAll(v, "\n", " "), "\r", " ")
}
