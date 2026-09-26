package nats

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestMonitorCheck(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/jsz":
			_, _ = w.Write([]byte(`{"streams":3,"consumers":5,"messages":42,"bytes":2048}`))
		case "/varz":
			_, _ = w.Write([]byte(`{"connections":7,"uptime":"1h2m"}`))
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()

	monitor := NewMonitor(server.URL, time.Second)
	status, err := monitor.Check(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if !status.Reachable || status.Streams != 3 || status.Consumers != 5 || status.Connections != 7 {
		t.Fatalf("unexpected status: %+v", status)
	}
}
