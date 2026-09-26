package probe

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

type Result struct {
	Configured bool   `json:"configured"`
	Reachable  bool   `json:"reachable"`
	Detail     string `json:"detail"`
}

type Status struct {
	PostgreSQL Result `json:"postgresql"`
	NATS       Result `json:"nats"`
	MarketData Result `json:"marketData"`
}

type Config struct {
	PostgreSQLDSN string
	NATSURL       string
	MarketDataDB  string
	Timeout       time.Duration
}

type Prober struct {
	cfg Config
}

func New(cfg Config) *Prober {
	if cfg.Timeout <= 0 {
		cfg.Timeout = time.Second
	}
	return &Prober{cfg: cfg}
}

func (p *Prober) Check(ctx context.Context) Status {
	return Status{
		PostgreSQL: checkTCP(ctx, "PostgreSQL", postgresAddress(p.cfg.PostgreSQLDSN), p.cfg.PostgreSQLDSN != "", p.cfg.Timeout),
		NATS:       checkTCP(ctx, "NATS", natsAddress(p.cfg.NATSURL), p.cfg.NATSURL != "", p.cfg.Timeout),
		MarketData: checkSQLiteHeader(p.cfg.MarketDataDB),
	}
}

func checkTCP(ctx context.Context, label, address string, configured bool, timeout time.Duration) Result {
	if !configured {
		return Result{Configured: false, Reachable: false, Detail: label + " source is not configured"}
	}
	if address == "" {
		return Result{Configured: true, Reachable: false, Detail: label + " address could not be parsed"}
	}

	dialer := net.Dialer{Timeout: timeout}
	conn, err := dialer.DialContext(ctx, "tcp", address)
	if err != nil {
		return Result{Configured: true, Reachable: false, Detail: fmt.Sprintf("%s TCP probe failed: %v", label, err)}
	}
	_ = conn.Close()
	return Result{Configured: true, Reachable: true, Detail: label + " TCP endpoint reachable at " + address}
}

func postgresAddress(dsn string) string {
	dsn = strings.TrimSpace(dsn)
	if dsn == "" {
		return ""
	}
	if parsed, err := url.Parse(dsn); err == nil && parsed.Hostname() != "" {
		port := parsed.Port()
		if port == "" {
			port = "5432"
		}
		return net.JoinHostPort(parsed.Hostname(), port)
	}

	values := parseKeyValueDSN(dsn)
	host := values["host"]
	if host == "" {
		host = "localhost"
	}
	port := values["port"]
	if port == "" {
		port = "5432"
	}
	return net.JoinHostPort(host, port)
}

func parseKeyValueDSN(dsn string) map[string]string {
	result := make(map[string]string)
	for _, field := range strings.Fields(dsn) {
		key, value, ok := strings.Cut(field, "=")
		if !ok {
			continue
		}
		result[strings.TrimSpace(key)] = strings.Trim(strings.TrimSpace(value), "'\"")
	}
	return result
}

func natsAddress(raw string) string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return ""
	}
	parsed, err := url.Parse(raw)
	if err != nil || parsed.Hostname() == "" {
		return ""
	}
	port := parsed.Port()
	if port == "" {
		port = "4222"
	}
	return net.JoinHostPort(parsed.Hostname(), port)
}

func checkSQLiteHeader(path string) Result {
	path = strings.TrimSpace(path)
	if path == "" {
		return Result{Configured: false, Reachable: false, Detail: "market-data SQLite source is not configured"}
	}

	file, err := os.Open(path)
	if err != nil {
		return Result{Configured: true, Reachable: false, Detail: "market-data SQLite open failed: " + err.Error()}
	}
	defer file.Close()

	header := make([]byte, 16)
	if _, err := io.ReadFull(file, header); err != nil {
		return Result{Configured: true, Reachable: false, Detail: "market-data SQLite header read failed: " + err.Error()}
	}
	if string(header) != "SQLite format 3\x00" {
		return Result{Configured: true, Reachable: false, Detail: "market-data file is not a SQLite 3 database"}
	}

	info, err := file.Stat()
	if err != nil {
		return Result{Configured: true, Reachable: true, Detail: "market-data SQLite database is readable"}
	}
	return Result{Configured: true, Reachable: true, Detail: "market-data SQLite database is readable (" + strconv.FormatInt(info.Size(), 10) + " bytes)"}
}
