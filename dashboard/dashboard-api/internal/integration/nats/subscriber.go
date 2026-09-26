package nats

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// SubjectSubscriber is a tiny read-only NATS protocol client used only to
// invalidate dashboard read models. It never publishes, ACKs, creates
// consumers, or mutates trading state.
type SubjectSubscriber struct {
	natsURL  string
	timeout  time.Duration
	subjects []string
}

func NewSubjectSubscriber(natsURL string, subjects []string, timeout time.Duration) *SubjectSubscriber {
	clean := make([]string, 0, len(subjects))
	seen := make(map[string]struct{}, len(subjects))
	for _, subject := range subjects {
		subject = strings.TrimSpace(subject)
		if subject == "" {
			continue
		}
		if _, ok := seen[subject]; ok {
			continue
		}
		seen[subject] = struct{}{}
		clean = append(clean, subject)
	}
	if timeout <= 0 {
		timeout = 1500 * time.Millisecond
	}
	return &SubjectSubscriber{natsURL: strings.TrimSpace(natsURL), timeout: timeout, subjects: clean}
}

// Run keeps a subscription alive until ctx is cancelled. Transient NATS
// failures are retried with bounded backoff so the browser does not need to
// reconnect simply because NATS restarted.
func (s *SubjectSubscriber) Run(ctx context.Context, onSubject func(string)) error {
	if strings.TrimSpace(s.natsURL) == "" {
		return errors.New("NATS URL is not configured")
	}
	if len(s.subjects) == 0 {
		return errors.New("no NATS subjects configured for dashboard stream")
	}
	if onSubject == nil {
		return errors.New("NATS subject callback is required")
	}

	backoff := 250 * time.Millisecond
	for ctx.Err() == nil {
		err := s.runOnce(ctx, onSubject)
		if ctx.Err() != nil {
			return nil
		}
		if err == nil {
			backoff = 250 * time.Millisecond
			continue
		}

		timer := time.NewTimer(backoff)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil
		case <-timer.C:
		}
		if backoff < 5*time.Second {
			backoff *= 2
			if backoff > 5*time.Second {
				backoff = 5 * time.Second
			}
		}
	}
	return nil
}

func (s *SubjectSubscriber) runOnce(ctx context.Context, onSubject func(string)) error {
	parsed, err := url.Parse(s.natsURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return errors.New("invalid NATS URL")
	}
	if parsed.Scheme != "nats" {
		return fmt.Errorf("unsupported NATS scheme %q for dashboard stream", parsed.Scheme)
	}
	host := parsed.Host
	if !strings.Contains(host, ":") {
		host += ":4222"
	}

	dialer := &net.Dialer{Timeout: s.timeout}
	conn, err := dialer.DialContext(ctx, "tcp", host)
	if err != nil {
		return fmt.Errorf("connect dashboard stream to NATS: %w", err)
	}
	defer conn.Close()

	stopCloser := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-stopCloser:
		}
	}()
	defer close(stopCloser)

	_ = conn.SetDeadline(time.Now().Add(s.timeout))
	reader := bufio.NewReader(conn)
	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			return fmt.Errorf("read NATS INFO for dashboard stream: %w", err)
		}
		if strings.HasPrefix(line, "INFO ") {
			break
		}
		if line == "PING" {
			if _, err := io.WriteString(conn, "PONG\r\n"); err != nil {
				return err
			}
		}
	}

	connect := map[string]any{
		"verbose":      false,
		"pedantic":     false,
		"tls_required": false,
		"name":         "control-dashboard-sse-bridge",
		"lang":         "go",
		"version":      "0.43.0",
		"protocol":     1,
		"echo":         false,
		"headers":      false,
	}
	if parsed.User != nil {
		connect["user"] = parsed.User.Username()
		if password, ok := parsed.User.Password(); ok {
			connect["pass"] = password
		}
	}
	connectJSON, _ := json.Marshal(connect)
	if _, err := fmt.Fprintf(conn, "CONNECT %s\r\n", connectJSON); err != nil {
		return fmt.Errorf("initialize dashboard NATS stream: %w", err)
	}
	for index, subject := range s.subjects {
		if _, err := fmt.Fprintf(conn, "SUB %s %d\r\n", subject, index+1); err != nil {
			return fmt.Errorf("subscribe dashboard NATS subject %s: %w", subject, err)
		}
	}
	if _, err := io.WriteString(conn, "PING\r\n"); err != nil {
		return fmt.Errorf("flush dashboard NATS subscriptions: %w", err)
	}

	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			return fmt.Errorf("await dashboard NATS subscription PONG: %w", err)
		}
		switch {
		case line == "PONG":
			_ = conn.SetDeadline(time.Time{})
			goto streaming
		case line == "PING":
			_, _ = io.WriteString(conn, "PONG\r\n")
		case strings.HasPrefix(line, "-ERR"):
			return fmt.Errorf("NATS dashboard stream rejected: %s", line)
		}
	}

streaming:
	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return fmt.Errorf("read dashboard NATS stream: %w", err)
		}
		switch {
		case line == "PING":
			if _, err := io.WriteString(conn, "PONG\r\n"); err != nil {
				return fmt.Errorf("reply dashboard NATS PING: %w", err)
			}
		case strings.HasPrefix(line, "-ERR"):
			return fmt.Errorf("NATS dashboard stream error: %s", line)
		case strings.HasPrefix(line, "MSG "):
			parts := strings.Fields(line)
			if len(parts) != 4 && len(parts) != 5 {
				return fmt.Errorf("unexpected NATS MSG frame in dashboard stream")
			}
			size, err := strconv.Atoi(parts[len(parts)-1])
			if err != nil || size < 0 || size > 8<<20 {
				return fmt.Errorf("invalid NATS dashboard stream payload size")
			}
			body := make([]byte, size+2)
			if _, err := io.ReadFull(reader, body); err != nil {
				return fmt.Errorf("read dashboard NATS MSG payload: %w", err)
			}
			if string(body[size:]) != "\r\n" {
				return fmt.Errorf("invalid NATS dashboard stream payload terminator")
			}
			onSubject(parts[1])
		}
	}
}
