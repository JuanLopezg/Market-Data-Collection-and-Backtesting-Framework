package hyperliquid

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestPublicClientChecksMetaAndMidsWithoutAuth(t *testing.T) {
	requests := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests++
		if r.Method != http.MethodPost || r.URL.Path != "/info" {
			t.Fatalf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		if r.Header.Get("Authorization") != "" {
			t.Fatal("public probe must not send Authorization")
		}
		var input map[string]any
		if err := json.NewDecoder(r.Body).Decode(&input); err != nil {
			t.Fatal(err)
		}
		w.Header().Set("Content-Type", "application/json")
		switch input["type"] {
		case "meta":
			_, _ = w.Write([]byte(`{"universe":[{"name":"BTC"},{"name":"ETH"},{"name":"SOL"}]}`))
		case "allMids":
			_, _ = w.Write([]byte(`{"BTC":"100000","ETH":"4000","SOL":"200"}`))
		default:
			http.Error(w, "unexpected info type", http.StatusBadRequest)
		}
	}))
	defer server.Close()

	client := NewPublicClient(server.URL+"/info", time.Second)
	got, err := client.Check(context.Background())
	if err != nil {
		t.Fatalf("Check() error = %v", err)
	}
	if !got.Connected || !got.MetadataReady || !got.MidsReady {
		t.Fatalf("unexpected public status: %+v", got)
	}
	if got.UniverseCount != 3 || got.MidCount != 3 || got.MatchedMidCount != 3 {
		t.Fatalf("unexpected counts: %+v", got)
	}
	if requests != 2 {
		t.Fatalf("requests = %d, want 2", requests)
	}
}

func TestPublicClientFailsClosedOnEmptyMetadata(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"universe":[]}`))
	}))
	defer server.Close()

	client := NewPublicClient(server.URL, time.Second)
	if _, err := client.Check(context.Background()); err == nil {
		t.Fatal("Check() accepted empty metadata")
	}
}

func TestPublicClientRefusesRedirect(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"universe":[{"name":"BTC"}]}`))
	}))
	defer target.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, target.URL, http.StatusFound)
	}))
	defer redirect.Close()

	client := NewPublicClient(redirect.URL, time.Second)
	if _, err := client.Check(context.Background()); err == nil {
		t.Fatal("Check() followed redirect")
	}
}
