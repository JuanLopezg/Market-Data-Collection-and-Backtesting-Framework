package server

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

type Role string

const (
	RoleViewer   Role = "VIEWER"
	RoleOperator Role = "OPERATOR"
)

type AuthUser struct {
	Username string
	Password string
	Role     Role
}

type session struct {
	Username  string
	Role      Role
	CSRFToken string
	ExpiresAt time.Time
}

type sessionStore struct {
	mu       sync.Mutex
	sessions map[string]session
	ttl      time.Duration
}

func newSessionStore(ttl time.Duration) *sessionStore {
	if ttl <= 0 {
		ttl = 8 * time.Hour
	}
	return &sessionStore{sessions: make(map[string]session), ttl: ttl}
}

func randomToken() (string, error) {
	buf := make([]byte, 32)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buf), nil
}

func (s *sessionStore) create(user AuthUser) (string, session, error) {
	token, err := randomToken()
	if err != nil {
		return "", session{}, err
	}
	csrf, err := randomToken()
	if err != nil {
		return "", session{}, err
	}
	now := time.Now().UTC()
	value := session{Username: user.Username, Role: user.Role, CSRFToken: csrf, ExpiresAt: now.Add(s.ttl)}
	s.mu.Lock()
	s.sessions[token] = value
	s.mu.Unlock()
	return token, value, nil
}

func (s *sessionStore) get(token string) (session, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	value, ok := s.sessions[token]
	if !ok {
		return session{}, false
	}
	if time.Now().UTC().After(value.ExpiresAt) {
		delete(s.sessions, token)
		return session{}, false
	}
	return value, true
}

func (s *sessionStore) delete(token string) {
	s.mu.Lock()
	delete(s.sessions, token)
	s.mu.Unlock()
}

type loginAttempt struct {
	windowStart time.Time
	failures    int
}

type loginLimiter struct {
	mu       sync.Mutex
	attempts map[string]loginAttempt
	limit    int
	window   time.Duration
}

func newLoginLimiter() *loginLimiter {
	return &loginLimiter{attempts: make(map[string]loginAttempt), limit: 5, window: time.Minute}
}

func (l *loginLimiter) allowed(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	attempt := l.attempts[key]
	if attempt.windowStart.IsZero() || now.Sub(attempt.windowStart) >= l.window {
		l.attempts[key] = loginAttempt{windowStart: now}
		return true
	}
	return attempt.failures < l.limit
}

func (l *loginLimiter) failed(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	attempt := l.attempts[key]
	if attempt.windowStart.IsZero() || now.Sub(attempt.windowStart) >= l.window {
		attempt = loginAttempt{windowStart: now}
	}
	attempt.failures++
	l.attempts[key] = attempt
}

func (l *loginLimiter) succeeded(key string) {
	l.mu.Lock()
	delete(l.attempts, key)
	l.mu.Unlock()
}

type authContextKey struct{}

func sessionFromContext(ctx context.Context) (session, bool) {
	value, ok := ctx.Value(authContextKey{}).(session)
	return value, ok
}

func secureEqual(left, right string) bool {
	lh := sha256.Sum256([]byte(left))
	rh := sha256.Sum256([]byte(right))
	return subtle.ConstantTimeCompare(lh[:], rh[:]) == 1
}

func clientKey(r *http.Request) string {
	if forwarded := r.Header.Get("X-Forwarded-For"); forwarded != "" {
		if first := strings.TrimSpace(strings.Split(forwarded, ",")[0]); first != "" {
			return first
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil && host != "" {
		return host
	}
	return r.RemoteAddr
}
