package nats

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

func TestSubjectSubscriberReceivesSubjectWithoutPublishing(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	serverErr := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			serverErr <- err
			return
		}
		defer conn.Close()
		_, _ = fmt.Fprint(conn, "INFO {\"server_id\":\"test\"}\r\n")
		reader := bufio.NewReader(conn)
		seenSub := false
		for {
			line, err := readProtocolLine(reader)
			if err != nil {
				serverErr <- err
				return
			}
			if strings.HasPrefix(line, "PUB ") {
				serverErr <- fmt.Errorf("subscriber unexpectedly published: %s", line)
				return
			}
			if strings.HasPrefix(line, "SUB market.data.updated.v1 ") {
				seenSub = true
			}
			if line == "PING" && seenSub {
				_, _ = fmt.Fprint(conn, "PONG\r\n")
				_, _ = fmt.Fprint(conn, "MSG market.data.updated.v1 1 2\r\n{}\r\n")
				serverErr <- nil
				return
			}
		}
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	got := make(chan string, 1)
	subscriber := NewSubjectSubscriber("nats://"+listener.Addr().String(), []string{"market.data.updated.v1"}, 500*time.Millisecond)
	go func() {
		_ = subscriber.Run(ctx, func(subject string) {
			select {
			case got <- subject:
			default:
			}
			cancel()
		})
	}()

	select {
	case subject := <-got:
		if subject != "market.data.updated.v1" {
			t.Fatalf("subject = %q", subject)
		}
	case <-time.After(1500 * time.Millisecond):
		t.Fatal("timed out waiting for dashboard NATS subject")
	}
	if err := <-serverErr; err != nil {
		t.Fatal(err)
	}
}
