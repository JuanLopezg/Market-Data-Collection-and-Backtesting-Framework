package hyperliquid

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestPublicClientPerpRulesUsesOnlyMetaAndAllMids(t *testing.T) {
	var types []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			t.Fatal("public rules probe must not send Authorization")
		}
		var input map[string]any
		if err := json.NewDecoder(r.Body).Decode(&input); err != nil {
			t.Fatal(err)
		}
		typ, _ := input["type"].(string)
		types = append(types, typ)
		w.Header().Set("Content-Type", "application/json")
		switch typ {
		case "meta":
			_, _ = w.Write([]byte(`{"universe":[{"name":"BTC","szDecimals":5,"maxLeverage":50},{"name":"DOGE","szDecimals":0,"maxLeverage":20,"onlyIsolated":true,"marginMode":"noCross"}]}`))
		case "allMids":
			_, _ = w.Write([]byte(`{"BTC":"100000.0","DOGE":"0.25"}`))
		default:
			http.Error(w, "unexpected", http.StatusBadRequest)
		}
	}))
	defer server.Close()

	client := NewPublicClient(server.URL, time.Second)
	got, _, _, err := client.PerpRules(context.Background())
	if err != nil {
		t.Fatalf("PerpRules() error = %v", err)
	}
	if len(got) != 2 || got[0].Name != "BTC" || got[0].SzDecimals != 5 || got[0].MaxLeverage != 50 || got[0].Mid != "100000.0" {
		t.Fatalf("unexpected rules: %+v", got)
	}
	if !got[1].OnlyIsolated || got[1].MarginMode != "noCross" {
		t.Fatalf("margin constraints lost: %+v", got[1])
	}
	if len(types) != 2 || types[0] != "meta" || types[1] != "allMids" {
		t.Fatalf("unexpected public info calls: %+v", types)
	}
}

func TestPublicClientPerpRulesFailsClosedOnDuplicateCoin(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var input map[string]any
		_ = json.NewDecoder(r.Body).Decode(&input)
		if input["type"] == "meta" {
			_, _ = w.Write([]byte(`{"universe":[{"name":"BTC","szDecimals":5,"maxLeverage":50},{"name":"BTC","szDecimals":5,"maxLeverage":50}]}`))
			return
		}
		_, _ = w.Write([]byte(`{"BTC":"100000"}`))
	}))
	defer server.Close()
	client := NewPublicClient(server.URL, time.Second)
	if _, _, _, err := client.PerpRules(context.Background()); err == nil {
		t.Fatal("PerpRules() accepted duplicate metadata coin")
	}
}
