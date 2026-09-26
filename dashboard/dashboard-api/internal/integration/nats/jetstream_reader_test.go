package nats

import (
	"bufio"
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"strings"
	"testing"
	"time"
)

type fakeRequester struct {
	subject  string
	payload  []byte
	response []byte
	err      error
}

func (f *fakeRequester) Request(_ context.Context, subject string, payload []byte) ([]byte, error) {
	f.subject = subject
	f.payload = append([]byte(nil), payload...)
	return f.response, f.err
}

func TestLastBySubjectDecodesJetStreamMessage(t *testing.T) {
	data := []byte(`{"snapshot":{"timestamp":1}}`)
	fake := &fakeRequester{response: []byte(`{"message":{"subject":"execution.exchange.snapshot.v1","seq":42,"data":"` + base64.StdEncoding.EncodeToString(data) + `","time":"2026-09-24T18:00:00Z"}}`)}
	reader := NewJetStreamReaderWithRequester("ALGOTRADING_RUNTIME", fake)
	message, err := reader.LastBySubject(context.Background(), SubjectExchangeSnapshot)
	if err != nil {
		t.Fatal(err)
	}
	if fake.subject != "$JS.API.STREAM.MSG.GET.ALGOTRADING_RUNTIME" || message.Sequence != 42 || string(message.Data) != string(data) {
		t.Fatalf("unexpected direct-get result: %+v request=%s", message, fake.subject)
	}
}

func TestLastBySubjectMapsNotFound(t *testing.T) {
	fake := &fakeRequester{response: []byte(`{"error":{"code":404,"err_code":10037,"description":"no message found"}}`)}
	reader := NewJetStreamReaderWithRequester("ALGOTRADING_RUNTIME", fake)
	_, err := reader.LastBySubject(context.Background(), SubjectExchangeSnapshot)
	if !errors.Is(err, ErrJetStreamMessageNotFound) {
		t.Fatalf("expected ErrJetStreamMessageNotFound, got %v", err)
	}
}

func TestRawNATSRequesterRequestProtocol(t *testing.T) {
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
		reader := bufio.NewReader(conn)
		if _, err := io.WriteString(conn, "INFO {\"server_id\":\"test\"}\r\n"); err != nil {
			serverErr <- err
			return
		}
		line, err := readProtocolLine(reader)
		if err != nil || !strings.HasPrefix(line, "CONNECT ") {
			serverErr <- fmt.Errorf("expected CONNECT, got %q err=%v", line, err)
			return
		}
		line, err = readProtocolLine(reader)
		if err != nil || line != "PING" {
			serverErr <- fmt.Errorf("expected PING, got %q err=%v", line, err)
			return
		}
		if _, err := io.WriteString(conn, "PONG\r\n"); err != nil {
			serverErr <- err
			return
		}
		subLine, err := readProtocolLine(reader)
		if err != nil {
			serverErr <- err
			return
		}
		parts := strings.Fields(subLine)
		if len(parts) != 3 || parts[0] != "SUB" || parts[2] != "1" {
			serverErr <- fmt.Errorf("unexpected SUB frame: %q", subLine)
			return
		}
		inbox := parts[1]
		pubLine, err := readProtocolLine(reader)
		if err != nil {
			serverErr <- err
			return
		}
		pub := strings.Fields(pubLine)
		if len(pub) != 4 || pub[0] != "PUB" || pub[1] != "$JS.API.STREAM.MSG.GET.ALGOTRADING_RUNTIME" || pub[2] != inbox {
			serverErr <- fmt.Errorf("unexpected PUB frame: %q", pubLine)
			return
		}
		size, err := strconv.Atoi(pub[3])
		if err != nil {
			serverErr <- err
			return
		}
		requestBody := make([]byte, size+2)
		if _, err := io.ReadFull(reader, requestBody); err != nil {
			serverErr <- err
			return
		}
		if !strings.Contains(string(requestBody[:size]), "last_by_subj") {
			serverErr <- fmt.Errorf("unexpected request body %q", requestBody[:size])
			return
		}
		line, err = readProtocolLine(reader)
		if err != nil || line != "PING" {
			serverErr <- fmt.Errorf("expected flush PING, got %q err=%v", line, err)
			return
		}
		response := []byte(`{"message":{"subject":"execution.exchange.snapshot.v1","seq":1,"data":"e30=","time":"2026-09-24T18:00:00Z"}}`)
		if _, err := fmt.Fprintf(conn, "MSG %s 1 %d\r\n", inbox, len(response)); err != nil {
			serverErr <- err
			return
		}
		if _, err := conn.Write(response); err != nil {
			serverErr <- err
			return
		}
		if _, err := io.WriteString(conn, "\r\nPONG\r\n"); err != nil {
			serverErr <- err
			return
		}
		serverErr <- nil
	}()

	requester := &rawNATSRequester{natsURL: "nats://" + listener.Addr().String(), timeout: time.Second}
	body, err := requester.Request(context.Background(), "$JS.API.STREAM.MSG.GET.ALGOTRADING_RUNTIME", []byte(`{"last_by_subj":"execution.exchange.snapshot.v1"}`))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), `"seq":1`) {
		t.Fatalf("unexpected response body: %s", body)
	}
	if err := <-serverErr; err != nil {
		t.Fatal(err)
	}
}
