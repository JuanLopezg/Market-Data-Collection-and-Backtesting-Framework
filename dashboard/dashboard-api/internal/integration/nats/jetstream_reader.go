package nats

import (
	"bufio"
	"context"
	"encoding/base64"
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

var ErrJetStreamMessageNotFound = errors.New("JetStream message not found")

type JetStreamMessage struct {
	Subject  string `json:"subject"`
	Sequence uint64 `json:"sequence"`
	Time     string `json:"time"`
	Data     []byte `json:"-"`
}

type NATSRequester interface {
	Request(ctx context.Context, subject string, payload []byte) ([]byte, error)
}

type JetStreamReader struct {
	stream    string
	requester NATSRequester
}

func NewJetStreamReader(natsURL, stream string, timeout time.Duration) *JetStreamReader {
	if strings.TrimSpace(stream) == "" {
		stream = "ALGOTRADING_RUNTIME"
	}
	return &JetStreamReader{
		stream:    strings.TrimSpace(stream),
		requester: &rawNATSRequester{natsURL: strings.TrimSpace(natsURL), timeout: timeout},
	}
}

func NewJetStreamReaderWithRequester(stream string, requester NATSRequester) *JetStreamReader {
	return &JetStreamReader{stream: stream, requester: requester}
}

func (r *JetStreamReader) LastBySubject(ctx context.Context, subject string) (JetStreamMessage, error) {
	var result JetStreamMessage
	if r.requester == nil {
		return result, errors.New("NATS requester is not configured")
	}
	if strings.TrimSpace(r.stream) == "" {
		return result, errors.New("JetStream stream is not configured")
	}
	subject = strings.TrimSpace(subject)
	if subject == "" {
		return result, errors.New("JetStream subject is required")
	}

	requestPayload, _ := json.Marshal(map[string]string{"last_by_subj": subject})
	apiSubject := "$JS.API.STREAM.MSG.GET." + r.stream
	raw, err := r.requester.Request(ctx, apiSubject, requestPayload)
	if err != nil {
		return result, err
	}

	var response struct {
		Error *struct {
			Code        int    `json:"code"`
			ErrCode     int    `json:"err_code"`
			Description string `json:"description"`
		} `json:"error"`
		Message *struct {
			Subject string `json:"subject"`
			Seq     uint64 `json:"seq"`
			Data    string `json:"data"`
			Time    string `json:"time"`
		} `json:"message"`
	}
	if err := json.Unmarshal(raw, &response); err != nil {
		return result, fmt.Errorf("decode JetStream direct-get response: %w", err)
	}
	if response.Error != nil {
		if response.Error.Code == 404 || response.Error.ErrCode == 10037 {
			return result, ErrJetStreamMessageNotFound
		}
		return result, fmt.Errorf("JetStream direct-get failed: code=%d err_code=%d description=%s", response.Error.Code, response.Error.ErrCode, response.Error.Description)
	}
	if response.Message == nil {
		return result, ErrJetStreamMessageNotFound
	}
	decoded, err := base64.StdEncoding.DecodeString(response.Message.Data)
	if err != nil {
		return result, fmt.Errorf("decode JetStream message data: %w", err)
	}
	result.Subject = response.Message.Subject
	result.Sequence = response.Message.Seq
	result.Time = response.Message.Time
	result.Data = decoded
	return result, nil
}

type rawNATSRequester struct {
	natsURL string
	timeout time.Duration
}

func (r *rawNATSRequester) Request(ctx context.Context, subject string, payload []byte) ([]byte, error) {
	if r.timeout <= 0 {
		r.timeout = 1500 * time.Millisecond
	}
	parsed, err := url.Parse(r.natsURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return nil, errors.New("invalid NATS URL")
	}
	if parsed.Scheme != "nats" {
		return nil, fmt.Errorf("unsupported NATS scheme %q for read-only direct-get", parsed.Scheme)
	}
	host := parsed.Host
	if !strings.Contains(host, ":") {
		host += ":4222"
	}

	dialer := &net.Dialer{Timeout: r.timeout}
	conn, err := dialer.DialContext(ctx, "tcp", host)
	if err != nil {
		return nil, fmt.Errorf("connect to NATS for JetStream read: %w", err)
	}
	defer conn.Close()

	deadline := time.Now().Add(r.timeout)
	if dl, ok := ctx.Deadline(); ok && dl.Before(deadline) {
		deadline = dl
	}
	_ = conn.SetDeadline(deadline)
	reader := bufio.NewReader(conn)

	// The server starts with INFO. Accept interleaved PINGs defensively.
	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			return nil, fmt.Errorf("read NATS INFO: %w", err)
		}
		if strings.HasPrefix(line, "INFO ") {
			break
		}
		if line == "PING" {
			if _, err := io.WriteString(conn, "PONG\r\n"); err != nil {
				return nil, err
			}
		}
	}

	connect := map[string]any{
		"verbose":      false,
		"pedantic":     false,
		"tls_required": false,
		"name":         "control-dashboard-readonly",
		"lang":         "go",
		"version":      "0.31.0",
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
	if _, err := fmt.Fprintf(conn, "CONNECT %s\r\nPING\r\n", connectJSON); err != nil {
		return nil, fmt.Errorf("initialize NATS connection: %w", err)
	}
	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			return nil, fmt.Errorf("await NATS PONG: %w", err)
		}
		if line == "PONG" {
			break
		}
		if line == "PING" {
			_, _ = io.WriteString(conn, "PONG\r\n")
			continue
		}
		if strings.HasPrefix(line, "-ERR") {
			return nil, fmt.Errorf("NATS connection rejected: %s", line)
		}
	}

	inbox := "_INBOX.control_dashboard." + strconv.FormatInt(time.Now().UnixNano(), 36)
	if _, err := fmt.Fprintf(conn, "SUB %s 1\r\nPUB %s %s %d\r\n", inbox, subject, inbox, len(payload)); err != nil {
		return nil, fmt.Errorf("send NATS JetStream request: %w", err)
	}
	if _, err := conn.Write(payload); err != nil {
		return nil, fmt.Errorf("send NATS JetStream request payload: %w", err)
	}
	if _, err := io.WriteString(conn, "\r\nPING\r\n"); err != nil {
		return nil, fmt.Errorf("flush NATS JetStream request: %w", err)
	}

	for {
		line, err := readProtocolLine(reader)
		if err != nil {
			return nil, fmt.Errorf("read NATS JetStream response: %w", err)
		}
		switch {
		case line == "PING":
			_, _ = io.WriteString(conn, "PONG\r\n")
		case strings.HasPrefix(line, "-ERR"):
			return nil, fmt.Errorf("NATS JetStream request failed: %s", line)
		case strings.HasPrefix(line, "MSG "):
			parts := strings.Fields(line)
			if len(parts) != 4 && len(parts) != 5 {
				return nil, fmt.Errorf("unexpected NATS MSG frame")
			}
			size, err := strconv.Atoi(parts[len(parts)-1])
			if err != nil || size < 0 || size > 4<<20 {
				return nil, fmt.Errorf("invalid NATS MSG payload size")
			}
			body := make([]byte, size+2)
			if _, err := io.ReadFull(reader, body); err != nil {
				return nil, fmt.Errorf("read NATS MSG payload: %w", err)
			}
			if string(body[size:]) != "\r\n" {
				return nil, fmt.Errorf("invalid NATS MSG payload terminator")
			}
			return body[:size], nil
		}
	}
}

func readProtocolLine(reader *bufio.Reader) (string, error) {
	line, err := reader.ReadString('\n')
	if err != nil {
		return "", err
	}
	if len(line) > 64<<10 {
		return "", errors.New("NATS protocol line too large")
	}
	return strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r"), nil
}
