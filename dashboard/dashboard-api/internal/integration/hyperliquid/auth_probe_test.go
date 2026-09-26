package hyperliquid

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestUserRoleAgentResponse(t *testing.T) {
	master := "0x1111111111111111111111111111111111111111"
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body map[string]any
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Fatal(err)
		}
		if body["type"] != "userRole" {
			t.Fatalf("type=%v", body["type"])
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"role": "agent", "data": map[string]any{"user": master}})
	}))
	defer srv.Close()
	client := NewPublicClient(srv.URL, time.Second)
	got, _, _, err := client.UserRole(context.Background(), "0x2222222222222222222222222222222222222222")
	if err != nil {
		t.Fatal(err)
	}
	if got.Role != "agent" || got.Data.User != master {
		t.Fatalf("unexpected role: %+v", got)
	}
}
