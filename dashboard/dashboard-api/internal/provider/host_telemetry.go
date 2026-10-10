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
	Processes     []hostProcess   `json:"processes"`
}

type hostProcess struct {
	Service        string `json:"service"`
	ProcessRunning *bool  `json:"processRunning"`
	ProcessState   string `json:"processState"`
	Detail         string `json:"detail"`
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
	if result.SchemaVersion != 1 || result.VPS == nil || len(result.Containers) > 64 || len(result.Processes) > 64 ||
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
	seen := map[string]bool{}
	for _, row := range result.Processes {
		if row.Service == "" || len(row.Service) > 128 || seen[row.Service] || len(row.Detail) > 512 ||
			(row.ProcessState != "UNKNOWN" && row.ProcessState != "RUNNING" && row.ProcessState != "STOPPED" && row.ProcessState != "MISSING") ||
			(row.ProcessState == "UNKNOWN" && row.ProcessRunning != nil) ||
			(row.ProcessState != "UNKNOWN" && (row.ProcessRunning == nil || *row.ProcessRunning != (row.ProcessState == "RUNNING"))) {
			return hostTelemetry{}, errors.New("Trading process telemetry invalid")
		}
		seen[row.Service] = true
	}
	if !result.VPS.ClockObserved && result.VPS.ClockSynced {
		return hostTelemetry{}, errors.New("Clock synchronization lacks observation")
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
	for _, row := range snapshot.Processes {
		health := "UNKNOWN"
		if row.ProcessState == "RUNNING" {
			health = "HEALTHY"
		} else if row.ProcessState == "STOPPED" || row.ProcessState == "MISSING" {
			health = "CRITICAL"
		}
		result.Services = append(result.Services, realService{
			Service: row.Service, Ready: false, ProcessRunning: row.ProcessRunning, ProcessState: row.ProcessState,
			Mode: p.runtimeMode(), LastEvent: snapshot.ObservedAt, LagLabel: row.Detail, Health: health,
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
			if len(snapshot.Processes) == 0 && state != "CRITICAL" {
				state = "UNKNOWN"
			}
			for _, row := range snapshot.Processes {
				if row.ProcessState == "STOPPED" || row.ProcessState == "MISSING" {
					state = "CRITICAL"
					break
				}
				if row.ProcessState == "UNKNOWN" && state != "CRITICAL" {
					state = "UNKNOWN"
				}
			}
			for _, row := range snapshot.Containers {
				switch row.Name {
				case "market-data", "strategy", "portfolio-risk", "execution-state", "order-planner", "exchange-gateway", "simulated-exchange":
					observed := false
					for _, process := range snapshot.Processes {
						observed = observed || process.Service == row.Name
					}
					if !observed && state != "CRITICAL" {
						state = "UNKNOWN"
					}
				}
			}
			result.Dependencies[index].State = state
			result.Dependencies[index].Reason = "External trading executable observations; no in-loop heartbeat or trading readiness is inferred"
		case "Clock sync":
			state := "UNKNOWN"
			if snapshot.VPS.ClockObserved {
				state = "CRITICAL"
				if snapshot.VPS.ClockSynced {
					state = "HEALTHY"
				}
			}
			result.Dependencies[index].State = state
			result.Dependencies[index].Reason = snapshot.VPS.ClockOffsetLabel
		}
	}
}
