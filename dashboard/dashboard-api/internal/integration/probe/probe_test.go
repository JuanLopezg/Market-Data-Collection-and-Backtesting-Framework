package probe

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestPostgresAddressSupportsLibpqAndURL(t *testing.T) {
	if got := postgresAddress("host=postgres port=5433 dbname=x user=y"); got != "postgres:5433" {
		t.Fatalf("libpq address = %q", got)
	}
	if got := postgresAddress("postgres://u:p@db.example:5544/name"); got != "db.example:5544" {
		t.Fatalf("url address = %q", got)
	}
}

func TestNATSAddress(t *testing.T) {
	if got := natsAddress("nats://nats:4222"); got != "nats:4222" {
		t.Fatalf("nats address = %q", got)
	}
}

func TestTCPProbe(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	result := checkTCP(context.Background(), "test", listener.Addr().String(), true, time.Second)
	if !result.Reachable {
		t.Fatalf("expected reachable: %+v", result)
	}
}

func TestSQLiteHeader(t *testing.T) {
	path := filepath.Join(t.TempDir(), "database.db")
	if err := os.WriteFile(path, append([]byte("SQLite format 3\x00"), make([]byte, 32)...), 0600); err != nil {
		t.Fatal(err)
	}
	result := checkSQLiteHeader(path)
	if !result.Reachable {
		t.Fatalf("expected readable SQLite source: %+v", result)
	}
}
