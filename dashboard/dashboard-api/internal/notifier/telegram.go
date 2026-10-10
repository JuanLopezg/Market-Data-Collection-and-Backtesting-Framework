package notifier

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	TelegramReceiptVersion  = "step46c-telegram-receipt-v1"
	telegramAPIBaseURL      = "https://api.telegram.org"
	maxTelegramResponseBody = int64(1 << 20)
	maxTelegramTextRunes    = 4000
	maxTelegramReceiptLine  = 16 * 1024
	maxTelegramReceiptRead  = int64(32 << 20)
)

type TelegramReceipt struct {
	Version        string `json:"version"`
	NotificationID string `json:"notificationId"`
	SourceEventID  string `json:"sourceEventId"`
	SentAt         string `json:"sentAt"`
	MessageID      int64  `json:"messageId"`
}

type TelegramSink struct {
	mu             sync.Mutex
	botToken       string
	chatID         string
	receiptPath    string
	baseURL        string
	client         *http.Client
	now            func() time.Time
	retryNotBefore time.Time
}

type telegramSendMessageRequest struct {
	ChatID             string                     `json:"chat_id"`
	Text               string                     `json:"text"`
	LinkPreviewOptions telegramLinkPreviewOptions `json:"link_preview_options"`
}

type telegramLinkPreviewOptions struct {
	IsDisabled bool `json:"is_disabled"`
}

type telegramAPIResponse struct {
	OK          bool   `json:"ok"`
	ErrorCode   int    `json:"error_code,omitempty"`
	Description string `json:"description,omitempty"`
	Result      struct {
		MessageID int64 `json:"message_id"`
	} `json:"result,omitempty"`
	Parameters struct {
		RetryAfter int `json:"retry_after,omitempty"`
	} `json:"parameters,omitempty"`
}

func NewTelegramSink(botToken, chatID, receiptPath string, timeout time.Duration) (*TelegramSink, error) {
	if timeout < time.Second || timeout > time.Minute {
		return nil, errors.New("telegram request timeout must be between 1s and 1m")
	}
	client := &http.Client{
		Timeout: timeout,
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	return newTelegramSink(botToken, chatID, receiptPath, telegramAPIBaseURL, client)
}

func newTelegramSink(botToken, chatID, receiptPath, baseURL string, client *http.Client) (*TelegramSink, error) {
	botToken = strings.TrimSpace(botToken)
	chatID = strings.TrimSpace(chatID)
	receiptPath = strings.TrimSpace(receiptPath)
	baseURL = strings.TrimRight(strings.TrimSpace(baseURL), "/")
	if err := validateTelegramToken(botToken); err != nil {
		return nil, err
	}
	if err := validateTelegramChatID(chatID); err != nil {
		return nil, err
	}
	if receiptPath == "" {
		return nil, errors.New("telegram receipt path is empty")
	}
	if baseURL == "" {
		return nil, errors.New("telegram API base URL is empty")
	}
	if client == nil {
		return nil, errors.New("telegram HTTP client is nil")
	}
	return &TelegramSink{
		botToken:    botToken,
		chatID:      chatID,
		receiptPath: receiptPath,
		baseURL:     baseURL,
		client:      client,
		now:         time.Now,
	}, nil
}

func (s *TelegramSink) Name() string { return "TELEGRAM" }

func (s *TelegramSink) Deliver(ctx context.Context, n Notification) (DeliveryResult, error) {
	select {
	case <-ctx.Done():
		return DeliveryResult{}, ctx.Err()
	default:
	}
	if strings.TrimSpace(n.NotificationID) == "" || strings.TrimSpace(n.SourceEventID) == "" || strings.TrimSpace(n.AlertID) == "" {
		return DeliveryResult{}, errors.New("notification requires notificationId, sourceEventId and alertId")
	}
	if n.Version == "" {
		n.Version = NotificationVersion
	}
	if n.Version != NotificationVersion {
		return DeliveryResult{}, fmt.Errorf("unsupported notification version %q", n.Version)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	exists, err := telegramReceiptContains(s.receiptPath, n.NotificationID)
	if err != nil {
		return DeliveryResult{}, err
	}
	if exists {
		return DeliveryResult{AlreadyDelivered: true}, nil
	}
	// Telegram's flood-control delay applies to this sink, including other alerts.
	// Leave the event unprocessed so the engine can retry after the deadline.
	if s.now().Before(s.retryNotBefore) {
		return DeliveryResult{}, fmt.Errorf("Telegram delivery deferred until %s after rate limiting", s.retryNotBefore.UTC().Format(time.RFC3339))
	}

	payload := telegramSendMessageRequest{
		ChatID: s.chatID,
		Text:   telegramMessage(n),
		LinkPreviewOptions: telegramLinkPreviewOptions{
			IsDisabled: true,
		},
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return DeliveryResult{}, errors.New("encode Telegram sendMessage request")
	}

	endpoint := s.baseURL + "/bot" + s.botToken + "/sendMessage"
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(body))
	if err != nil {
		return DeliveryResult{}, errors.New("build Telegram sendMessage request")
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "algoTrading-control-dashboard-notifier/step46c")

	resp, err := s.client.Do(req)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) || errors.Is(ctx.Err(), context.DeadlineExceeded) {
			return DeliveryResult{}, errors.New("Telegram sendMessage request timed out")
		}
		if errors.Is(err, context.Canceled) || errors.Is(ctx.Err(), context.Canceled) {
			return DeliveryResult{}, context.Canceled
		}
		// net/http transport errors frequently include the full request URL. The
		// bot token is part of that URL, so the raw error must never be returned
		// to logs/status.json.
		return DeliveryResult{}, errors.New("Telegram sendMessage transport failure")
	}
	defer resp.Body.Close()

	raw, err := io.ReadAll(io.LimitReader(resp.Body, maxTelegramResponseBody+1))
	if err != nil {
		return DeliveryResult{}, errors.New("read Telegram sendMessage response")
	}
	if int64(len(raw)) > maxTelegramResponseBody {
		return DeliveryResult{}, errors.New("Telegram sendMessage response exceeded bounded read limit")
	}

	var api telegramAPIResponse
	if err := json.Unmarshal(raw, &api); err != nil {
		return DeliveryResult{}, fmt.Errorf("Telegram sendMessage returned invalid JSON (HTTP %d)", resp.StatusCode)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 || !api.OK {
		desc := redactTelegramSecrets(api.Description, s.botToken, s.chatID)
		if len([]rune(desc)) > 240 {
			desc = string([]rune(desc)[:240]) + "…"
		}
		suffix := ""
		if api.Parameters.RetryAfter > 0 {
			suffix = fmt.Sprintf(" retry_after=%ds", api.Parameters.RetryAfter)
			// Guard duration conversion against an invalid overflowing remote value.
			seconds := int64(api.Parameters.RetryAfter)
			if seconds > int64((1<<63-1)/int64(time.Second)) {
				return DeliveryResult{}, errors.New("Telegram retry delay exceeds supported duration")
			}
			s.retryNotBefore = s.now().Add(time.Duration(seconds) * time.Second)
		}
		if desc != "" {
			suffix += ": " + desc
		}
		return DeliveryResult{}, fmt.Errorf("Telegram sendMessage rejected (HTTP %d error_code=%d)%s", resp.StatusCode, api.ErrorCode, suffix)
	}
	if api.Result.MessageID <= 0 {
		return DeliveryResult{}, errors.New("Telegram sendMessage success response missing message_id")
	}

	receipt := TelegramReceipt{
		Version:        TelegramReceiptVersion,
		NotificationID: n.NotificationID,
		SourceEventID:  n.SourceEventID,
		SentAt:         time.Now().UTC().Format(time.RFC3339Nano),
		MessageID:      api.Result.MessageID,
	}
	if err := appendTelegramReceipt(s.receiptPath, receipt); err != nil {
		return DeliveryResult{}, err
	}
	return DeliveryResult{}, nil
}

func telegramMessage(n Notification) string {
	severity := strings.ToUpper(strings.TrimSpace(n.Severity))
	transition := strings.ToUpper(strings.TrimSpace(n.Transition))
	if severity == "CRITICAL" {
		severity = "URGENT"
	}
	if transition == "RESOLVED" {
		severity = "INFO"
	}
	if severity == "" {
		severity = "UNKNOWN"
	}
	title := cleanTelegramField(n.Title)
	if title == "" {
		title = "Alert notification"
	}

	if transition == "RESOLVED" {
		title = "Resolved: " + strings.TrimSuffix(title, " resolved")
	} else if transition == "TEST" {
		title = "TEST: " + title
	}
	account := cleanTelegramField(n.Account)
	if account == "" && n.Service == "KrakenAccountReadOnly" {
		account = "Kraken"
	}
	owner := "System: " + cleanTelegramField(n.Service)
	if account != "" {
		owner = "Account: " + account
	} else if strings.TrimSpace(n.Service) == "" {
		owner = "System: algoTrading"
	}
	lines := []string{fmt.Sprintf("[%s] %s", severity, owner), truncateRunes(title, 160)}
	if v := cleanTelegramField(n.Detail); v != "" {
		// Daily portfolios need room for positions; operational alerts stay short.
		if n.EventType != "DAILY_PORTFOLIO" {
			v = truncateRunes(v, 600)
		}
		lines = append(lines, "", v)
	}
	return truncateRunes(strings.Join(lines, "\n"), maxTelegramTextRunes)
}

func cleanTelegramField(v string) string {
	v = strings.TrimSpace(strings.ReplaceAll(v, "\r\n", "\n"))
	v = strings.ReplaceAll(v, "\r", "\n")
	return strings.Map(func(r rune) rune {
		if r == '\n' || r == '\t' || r >= 0x20 {
			return r
		}
		return -1
	}, v)
}

func truncateRunes(v string, limit int) string {
	if limit <= 0 || v == "" {
		return ""
	}
	if utf8.RuneCountInString(v) <= limit {
		return v
	}
	runes := []rune(v)
	if limit == 1 {
		return "…"
	}
	return string(runes[:limit-1]) + "…"
}

func validateTelegramToken(v string) error {
	if v == "" {
		return errors.New("Telegram bot token is empty")
	}
	if len(v) > 512 || !strings.Contains(v, ":") || strings.ContainsAny(v, "/?# \t\r\n") {
		return errors.New("Telegram bot token has invalid format")
	}
	return nil
}

func validateTelegramChatID(v string) error {
	if v == "" {
		return errors.New("Telegram chat id is empty")
	}
	if len(v) > 256 || strings.ContainsAny(v, "\r\n\x00") {
		return errors.New("Telegram chat id has invalid format")
	}
	return nil
}

func redactTelegramSecrets(v, token, chatID string) string {
	v = strings.ReplaceAll(v, token, "[REDACTED_TOKEN]")
	v = strings.ReplaceAll(v, chatID, "[REDACTED_CHAT]")
	return v
}

func telegramReceiptContains(path, notificationID string) (bool, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("open Telegram receipt journal: %w", err)
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return false, fmt.Errorf("stat Telegram receipt journal: %w", err)
	}
	if stat.Size() > maxTelegramReceiptRead {
		return false, fmt.Errorf("Telegram receipt journal exceeds bounded read limit (%d bytes)", maxTelegramReceiptRead)
	}
	scanner := bufio.NewScanner(io.LimitReader(f, maxTelegramReceiptRead+1))
	scanner.Buffer(make([]byte, 4096), maxTelegramReceiptLine+1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		var receipt TelegramReceipt
		if err := json.Unmarshal([]byte(line), &receipt); err != nil {
			return false, fmt.Errorf("decode Telegram receipt journal: %w", err)
		}
		if receipt.Version != TelegramReceiptVersion || receipt.NotificationID == "" || receipt.SourceEventID == "" || receipt.MessageID <= 0 {
			return false, errors.New("invalid Telegram receipt contract")
		}
		if receipt.NotificationID == notificationID {
			return true, nil
		}
	}
	if err := scanner.Err(); err != nil {
		return false, fmt.Errorf("read Telegram receipt journal: %w", err)
	}
	return false, nil
}

func appendTelegramReceipt(path string, receipt TelegramReceipt) error {
	if receipt.Version == "" {
		receipt.Version = TelegramReceiptVersion
	}
	if receipt.Version != TelegramReceiptVersion || receipt.NotificationID == "" || receipt.SourceEventID == "" || receipt.MessageID <= 0 {
		return errors.New("invalid Telegram receipt")
	}
	body, err := json.Marshal(receipt)
	if err != nil {
		return errors.New("encode Telegram receipt")
	}
	if len(body) > maxTelegramReceiptLine {
		return fmt.Errorf("Telegram receipt exceeds %d bytes", maxTelegramReceiptLine)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("create Telegram receipt directory: %w", err)
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o640)
	if err != nil {
		return fmt.Errorf("open Telegram receipt journal: %w", err)
	}
	defer f.Close()
	if _, err := f.Write(append(body, '\n')); err != nil {
		return fmt.Errorf("append Telegram receipt: %w", err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync Telegram receipt: %w", err)
	}
	return nil
}
