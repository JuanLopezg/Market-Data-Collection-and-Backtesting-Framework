package provider

import (
	"encoding/json"
	"errors"
	"io"
	"math"
	"os"
	"time"
)

// The host collector owns Docker access. The API gets only a read-only, bounded
// snapshot file and never needs the Docker socket or host command execution.
type hostTelemetry struct {
	SchemaVersion int             `json:"schemaVersion"`
	Project       string          `json:"project"`
	ObservedAt    string          `json:"observedAt"`
	Scope         string          `json:"scope"`
	VPS           *realVPS        `json:"vps"`
	Containers    []realContainer `json:"containers"`
	Errors        []string        `json:"errors"`
}

func readHostTelemetry(path, project string, now time.Time) (hostTelemetry, error) {
	var result hostTelemetry
	file, err := os.Open(path)
	if err != nil {
		return result, errors.New("Host telemetry file unavailable")
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 128*1024+1))
	if err != nil || len(data) > 128*1024 || json.Unmarshal(data, &result) != nil {
		return hostTelemetry{}, errors.New("Host telemetry snapshot invalid")
	}
	observed, err := time.Parse(time.RFC3339Nano, result.ObservedAt)
	if err != nil || now.Sub(observed) > 35*time.Second || observed.After(now.Add(5*time.Second)) {
		return hostTelemetry{}, errors.New("Host telemetry is stale or misdated")
	}
	if result.SchemaVersion != 1 || result.VPS == nil || len(result.Containers) > 64 ||
		(project != "" && result.Project != project) {
		return hostTelemetry{}, errors.New("Host telemetry identity or schema invalid")
	}
	var document map[string]json.RawMessage
	var host map[string]json.RawMessage
	json.Unmarshal(data, &document)
	json.Unmarshal(document["vps"], &host)
	for _, name := range []string{"cpuPct", "ramPct", "diskPct"} {
		var number *float64
		if json.Unmarshal(host[name], &number) != nil || number == nil || math.IsNaN(*number) ||
			math.IsInf(*number, 0) || *number < 0 || *number > 100 {
			return hostTelemetry{}, errors.New("Host telemetry measurements invalid")
		}
	}
	if result.VPS.State != "HEALTHY" && result.VPS.State != "WARN" && result.VPS.State != "CRITICAL" {
		return hostTelemetry{}, errors.New("Host telemetry state invalid")
	}
	for _, row := range result.Containers {
		if row.Name == "" || len(row.Name) > 128 || row.CPUPct < -1 || row.RAMMB < -1 ||
			(row.State != "RUNNING" && row.State != "STOPPED" && row.State != "RESTARTING") ||
			(row.Health != "HEALTHY" && row.Health != "WARN" && row.Health != "CRITICAL" && row.Health != "UNKNOWN") {
			return hostTelemetry{}, errors.New("Container telemetry invalid")
		}
	}
	return result, nil
}

func (p *Real) applyHostTelemetry(result *realInfrastructure, now time.Time) {
	if p.cfg.HostMetricsFile == "" {
		return
	}
	snapshot, err := readHostTelemetry(p.cfg.HostMetricsFile, p.cfg.HostMetricsProject, now)
	if err != nil {
		result.SourceNote += " " + err.Error() + "; host/container values remain unavailable."
		return
	}
	result.VPS = *snapshot.VPS
	result.Containers = snapshot.Containers
	result.TelemetryObservedAt = snapshot.ObservedAt
	result.SourceNote += " " + snapshot.Scope + ". Host CPU is 0–100% of total capacity; container CPU 100% is one core. Container liveness does not prove trading readiness."
	for _, row := range snapshot.Containers {
		running := row.State == "RUNNING"
		result.Services = append(result.Services, realService{
			Service: row.Name, Ready: false, ProcessRunning: &running,
			Mode: p.runtimeMode(), LastEvent: snapshot.ObservedAt, LagLabel: "Process/container observation; pipeline progress is separate", Health: row.Health,
		})
	}
	for index := range result.Dependencies {
		switch result.Dependencies[index].Component {
		case "Trading service liveness":
			state := "HEALTHY"
			if len(snapshot.Errors) > 0 || len(snapshot.Containers) == 0 {
				state = "UNKNOWN"
			}
			for _, row := range snapshot.Containers {
				if row.State != "RUNNING" || row.Health == "CRITICAL" {
					state = "CRITICAL"
					break
				}
				if row.Health == "WARN" {
					state = "WARN"
				}
			}
			result.Dependencies[index].State = state
			result.Dependencies[index].Reason = "Project-scoped container observation; business progress/readiness is separate"
		}
	}
}
