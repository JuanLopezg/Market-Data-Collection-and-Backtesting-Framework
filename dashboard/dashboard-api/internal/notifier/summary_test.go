package notifier

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"control-dashboard-api/internal/alertstore"
)

type summaryRecorder struct {
	separate  bool
	delivered []Notification
	summaries map[bool][]string // critical chat -> summaries in order
}

func (s *summaryRecorder) Name() string { return "RECORDER" }
func (s *summaryRecorder) Deliver(_ context.Context, n Notification) (DeliveryResult, error) {
	s.delivered = append(s.delivered, n)
	return DeliveryResult{}, nil
}
func (s *summaryRecorder) SeparateCriticalChat() bool { return s.separate }
func (s *summaryRecorder) SendSummary(_ context.Context, critical bool, text string) error {
	s.summaries[critical] = append(s.summaries[critical], text)
	return nil
}

func summaryEngine(t *testing.T, separate bool) (*Engine, *summaryRecorder) {
	t.Helper()
	store, err := OpenStore(filepath.Join(t.TempDir(), "state"))
	if err != nil {
		t.Fatal(err)
	}
	sink := &summaryRecorder{separate: separate, summaries: map[bool][]string{}}
	engine, _ := NewEngine(store, sink, "WARN", 5*time.Minute)
	engine.SetSummarySource("Watchdog")
	if _, err := engine.Process(context.Background(), nil); err != nil { // empty bootstrap
		t.Fatal(err)
	}
	return engine, sink
}

func TestActiveAlertSummaryPerChat(t *testing.T) {
	engine, sink := summaryEngine(t, true)
	gateway := lifecycle("g1", "2026-10-17T13:00:00Z", "OPENED", "process-gateway", "CRITICAL", "down")
	gateway.Alert.Service, gateway.Alert.EventType = "gateway-kraken", "TRADING_PROCESS_UNAVAILABLE"
	reject := lifecycle("r1", "2026-10-17T13:00:05Z", "OPENED", "reject-1", "WARN", "rejected")
	reject.Alert.Asset, reject.Alert.EventType = "NEARUSDT", "ORDER_REJECT"
	events := []alertstore.LifecycleEvent{gateway, reject}
	stats, err := engine.Process(context.Background(), events)
	if err != nil || stats.Delivered != 2 || stats.Summaries != 2 {
		t.Fatalf("stats %+v %v", stats, err)
	}
	if got := sink.summaries[true][0]; got != "📋 Watchdog · Critical alerts now: 1\n🔴 gateway-kraken down" {
		t.Fatalf("critical summary %q", got)
	}
	if got := sink.summaries[false][0]; got != "📋 Watchdog · Active alerts now: 1\n🟡 NEARUSDT: order rejected" {
		t.Fatalf("normal summary %q", got)
	}

	// The gateway recovers: only the critical group changed, and it is now clear.
	resolved := lifecycle("g2", "2026-10-17T13:02:00Z", "RESOLVED", "process-gateway", "CRITICAL", "up")
	resolved.Alert.Service, resolved.Alert.EventType = "gateway-kraken", "TRADING_PROCESS_UNAVAILABLE"
	if _, err := engine.Process(context.Background(), append(events, resolved)); err != nil {
		t.Fatal(err)
	}
	if len(sink.summaries[true]) != 2 || len(sink.summaries[false]) != 1 {
		t.Fatalf("summaries %+v", sink.summaries)
	}
	if got := sink.summaries[true][1]; got != "📋 Watchdog · Critical alerts now: 0\n✅ All clear" {
		t.Fatalf("clear summary %q", got)
	}
	// No change, no summary.
	if stats, _ := engine.Process(context.Background(), append(events, resolved)); stats.Summaries != 0 {
		t.Fatalf("summary without a change: %+v", stats)
	}
}

func TestSingleChatSummaryMarksCriticalAndSkipsUnreported(t *testing.T) {
	engine, sink := summaryEngine(t, false)
	if err := engine.SetCriticalDelay(5 * time.Minute); err != nil {
		t.Fatal(err)
	}
	engine.now = func() time.Time { return time.Date(2026, 10, 17, 13, 1, 0, 0, time.UTC) }
	held := lifecycle("c1", "2026-10-17T13:00:00Z", "OPENED", "process-risk", "CRITICAL", "down")
	held.Alert.Service, held.Alert.EventType = "risk", "TRADING_PROCESS_UNAVAILABLE"
	warning := lifecycle("w1", "2026-10-17T13:00:30Z", "OPENED", "margin", "WARN", "close")
	warning.Alert.Title, warning.Alert.EventType = "Close to maintenance margin", "MAINTENANCE_DROP"
	if _, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{held, warning}); err != nil {
		t.Fatal(err)
	}
	// The critical alert is still inside its 5-minute delay: not reported, not listed.
	if got := sink.summaries[false][0]; got != "📋 Watchdog · Active alerts now: 1\n🟡 Close to maintenance margin" {
		t.Fatalf("summary %q", got)
	}
	engine.now = func() time.Time { return time.Date(2026, 10, 17, 13, 6, 0, 0, time.UTC) }
	if _, err := engine.Process(context.Background(), []alertstore.LifecycleEvent{held, warning}); err != nil {
		t.Fatal(err)
	}
	got := sink.summaries[false][1]
	if !strings.HasPrefix(got, "📋 Watchdog · Active alerts now: 2 (1 critical)") || !strings.Contains(got, "🔴 risk down") {
		t.Fatalf("combined summary %q", got)
	}
}

func TestSummaryShowsColorAndNameWithoutSeverityLabels(t *testing.T) {
	text := summaryText("Kraken", []activeAlert{
		{Label: "margin breach", Severity: "CRITICAL"},
		{Label: "reserve low", Severity: "WARN"},
		{Label: "information", Severity: "INFO"},
		{Label: "other", Severity: "NEW_LEVEL"},
	}, false, false)
	for _, line := range []string{
		"🔴 margin breach", "🟡 reserve low",
		"🔵 information", "⚪ other",
	} {
		if !strings.Contains(text, line) {
			t.Fatalf("summary missing %q: %q", line, text)
		}
	}
	if strings.Contains(strings.ToLower(text), "urgent") {
		t.Fatalf("old urgency label remains: %q", text)
	}
	if strings.ContainsAny(text, "[]") {
		t.Fatalf("summary contains a severity label: %q", text)
	}
}
