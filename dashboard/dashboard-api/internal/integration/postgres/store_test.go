//go:build cgo
// +build cgo

package postgres

import (
	"strings"
	"testing"
)

func TestHardenDSNAddsBoundedNetworkTimeouts(t *testing.T) {
	keyValue := hardenDSN("host=postgres dbname=algotrading user=reader password=x")
	if !strings.Contains(keyValue, "connect_timeout=2") || !strings.Contains(keyValue, "tcp_user_timeout=2000") {
		t.Fatalf("hardened key/value DSN missing timeouts: %q", keyValue)
	}

	url := hardenDSN("postgresql://reader:x@postgres/algotrading?sslmode=disable")
	if !strings.Contains(url, "connect_timeout=2") || !strings.Contains(url, "tcp_user_timeout=2000") {
		t.Fatalf("hardened URL DSN missing timeouts: %q", url)
	}
}

func TestDecodeFillRowPreservesPrecision(t *testing.T) {
	row := []string{"11", "7", "1", "20260924", "BTC", "0", "0.123456789123", "40000.123456789", "0.0123456789", "0.123456789123"}
	fill, err := decodeFillRow(row)
	if err != nil {
		t.Fatalf("decodeFillRow() error = %v", err)
	}
	if fill.Fill.FillID != 11 || fill.Fill.OrderID != 7 || fill.Fill.Coin != "BTC" {
		t.Fatalf("unexpected fill identity: %+v", fill)
	}
	if fill.Fill.Quantity != 0.123456789123 || fill.Fill.Price != 40000.123456789 {
		t.Fatalf("precision changed: %+v", fill)
	}
}
