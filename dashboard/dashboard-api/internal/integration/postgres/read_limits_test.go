package postgres

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type queryRunnerFunc func(context.Context, string, string) ([]byte, error)

func (f queryRunnerFunc) QueryJSON(ctx context.Context, dsn, sql string) ([]byte, error) {
	return f(ctx, dsn, sql)
}

func TestReaderTimeoutDefaultsAndOverrides(t *testing.T) {
	for _, tc := range []struct {
		name          string
		query         time.Duration
		connect       time.Duration
		wantQuery     time.Duration
		wantConnect   string
		wantStatement string
	}{
		{"defaults", 0, 0, 8 * time.Second, "PGCONNECT_TIMEOUT=5", "statement_timeout=8000"},
		{"overrides", 11 * time.Second, 6500 * time.Millisecond, 11 * time.Second, "PGCONNECT_TIMEOUT=7", "statement_timeout=11000"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			reader := NewReader(Config{Timeout: tc.query, ConnectTimeout: tc.connect})
			runner := reader.runner.(PSQLRunner)
			env, err := postgresEnv("host=fixture user=reader", runner.ConnectTimeout, runner.StatementTimeout)
			if err != nil {
				t.Fatal(err)
			}
			joined := strings.Join(env, "\n")
			if reader.timeout != tc.wantQuery || !strings.Contains(joined, tc.wantConnect+"\n") || !strings.Contains(joined, tc.wantStatement) {
				t.Fatalf("wrong timeout configuration: query=%s, env=%s", reader.timeout, joined)
			}
			if !strings.Contains(joined, "default_transaction_read_only=on") {
				t.Fatal("timeout configuration disabled read-only transactions")
			}
		})
	}
}

func TestQueryConcurrencyIsBoundedAcrossReaders(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	started := make(chan struct{}, 8)
	release := make(chan struct{})
	var wg sync.WaitGroup
	defer func() { close(release); wg.Wait() }()
	runner := queryRunnerFunc(func(ctx context.Context, _, _ string) ([]byte, error) {
		started <- struct{}{}
		select {
		case <-release:
			return []byte(`{}`), nil
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	})
	readers := []*Reader{NewReader(Config{Runner: runner}), NewReader(Config{Runner: runner})}
	errs := make(chan error, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := readers[i%2].query(ctx, fmt.Sprint(i))
			errs <- err
		}(i)
	}
	for i := 0; i < 3; i++ {
		select {
		case <-started:
		case <-ctx.Done():
			t.Fatal("three concurrent queries did not start")
		}
	}
	select {
	case <-started:
		t.Fatal("more than three queries entered the runner")
	case <-time.After(50 * time.Millisecond):
	}
	// Neither a cancelled waiter nor one whose query deadline expires may
	// enter the runner, and waiting must count against the query timeout.
	waiter := NewReader(Config{Timeout: 30 * time.Millisecond, Runner: queryRunnerFunc(func(context.Context, string, string) ([]byte, error) {
		t.Error("cancelled/timed-out waiter entered runner")
		return nil, nil
	})})
	if _, err := waiter.query(ctx, "waiting"); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("queued query did not time out: %v", err)
	}
	cancelled, stop := context.WithCancel(ctx)
	stop()
	if _, err := waiter.query(cancelled, "cancelled"); !errors.Is(err, context.Canceled) {
		t.Fatalf("queued query lost cancellation: %v", err)
	}
	// Release one query at a time; each completion admits exactly one waiter.
	for i := 0; i < 8; i++ {
		select {
		case release <- struct{}{}:
		case <-ctx.Done():
			t.Fatal("queries did not drain")
		}
		if err := <-errs; err != nil {
			t.Fatalf("bounded query failed: %v", err)
		}
	}
}

func TestRunningQueryHonorsDeadlineAndCancellation(t *testing.T) {
	for _, deadline := range []bool{true, false} {
		ctx, cancel := context.WithCancel(context.Background())
		reader := NewReader(Config{Timeout: 30 * time.Millisecond, Runner: queryRunnerFunc(func(ctx context.Context, _, _ string) ([]byte, error) {
			if !deadline {
				cancel()
			}
			<-ctx.Done()
			return nil, ctx.Err()
		})})
		_, err := reader.query(ctx, "test")
		cancel()
		want := context.Canceled
		if deadline {
			want = context.DeadlineExceeded
		}
		if !errors.Is(err, want) {
			t.Fatalf("query did not preserve cancellation reason: %v", err)
		}
	}
	// Both failure paths must return the semaphore permit.
	reader := NewReader(Config{Runner: queryRunnerFunc(func(context.Context, string, string) ([]byte, error) {
		return []byte(`{}`), nil
	})})
	if _, err := reader.query(context.Background(), "after cancellation"); err != nil {
		t.Fatal(err)
	}
}

func TestPSQLProcessIsKilledOnQueryTimeout(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "psql"), []byte("#!/bin/sh\nexec sleep 30\n"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	reader := NewReader(Config{DSN: "host=fixture user=reader", Timeout: 100 * time.Millisecond})
	started := time.Now()
	_, err := reader.query(context.Background(), "SELECT 1")
	if !errors.Is(err, context.DeadlineExceeded) || time.Since(started) > 3*time.Second {
		t.Fatalf("psql did not stop promptly on timeout: %v", err)
	}
}

func TestQueryCacheCoalescesConcurrentReadsAndIsScopedToOneSweep(t *testing.T) {
	var calls atomic.Int32
	started := make(chan struct{})
	release := make(chan struct{})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	runner := queryRunnerFunc(func(ctx context.Context, _, _ string) ([]byte, error) {
		if calls.Add(1) == 1 {
			close(started)
			select {
			case <-release:
			case <-ctx.Done():
				return nil, ctx.Err()
			}
		}
		return []byte(`{"value":1}`), nil
	})
	reader := NewReader(Config{Runner: runner})
	sweep := WithQueryCache(ctx)
	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			raw, err := reader.query(sweep, "same SQL")
			if err != nil || string(raw) != `{"value":1}` {
				t.Errorf("shared query: %s, %v", raw, err)
			}
		}()
	}
	select {
	case <-started:
	case <-ctx.Done():
		t.Fatal("query did not start")
	}
	cancelled, stop := context.WithCancel(sweep)
	stop()
	if _, err := reader.query(cancelled, "same SQL"); !errors.Is(err, context.Canceled) {
		t.Fatalf("cache waiter lost cancellation: %v", err)
	}
	close(release)
	wg.Wait()
	if calls.Load() != 1 {
		t.Fatalf("identical sweep reads ran %d times", calls.Load())
	}
	if _, err := reader.query(WithQueryCache(ctx), "same SQL"); err != nil || calls.Load() != 2 {
		t.Fatal("next sweep reused an old query result")
	}
}

func TestQueryCacheSharesFailuresWithoutMaskingNextSweepRecovery(t *testing.T) {
	var calls int
	failure := errors.New("fixture source unavailable")
	reader := NewReader(Config{Runner: queryRunnerFunc(func(context.Context, string, string) ([]byte, error) {
		calls++
		if calls == 1 {
			return nil, failure
		}
		return []byte(`{}`), nil
	})})
	sweep := WithQueryCache(context.Background())
	for i := 0; i < 3; i++ {
		if _, err := reader.query(sweep, "same SQL"); !errors.Is(err, failure) {
			t.Fatal("failed read was not shared within sweep")
		}
	}
	if calls != 1 {
		t.Fatalf("failed read ran %d times", calls)
	}
	if _, err := reader.query(WithQueryCache(context.Background()), "same SQL"); err != nil || calls != 2 {
		t.Fatalf("recovery was masked by old sweep failure: %v", err)
	}
}
