package main

import (
	"context"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"
)

func reviewConfig() Config {
	return Config{ListenAddress: "127.0.0.1", RDPPort: 33999, ControlPort: 0,
		VideoPort: 0, Certificate: "unused-test-cert", PrivateKey: "unused-test-key", Width: 64, Height: 64}
}

func TestCloseBeforeRun(t *testing.T) {
	g, err := NewGateway(reviewConfig(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { g.Close(); close(done) }()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("Close before Run hangs")
	}
}

func TestOccupiedRDPPortPropagatesError(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	cfg := reviewConfig()
	cfg.RDPPort = uint16(l.Addr().(*net.TCPAddr).Port)
	g, err := NewGateway(cfg, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := g.Run(ctx); err == nil {
		t.Fatal("RDP bind failure not returned as error")
	}
}

func TestAgentWriteRenewsDeadline(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	s, err := l.Accept()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	_ = s.SetWriteDeadline(time.Now().Add(-time.Millisecond))
	g := &Gateway{agent: &agentConnection{conn: s}}
	if err := g.sendAgent(controlPing, nil); err != nil {
		t.Fatalf("expired previous deadline not renewed: %v", err)
	}
	_ = c.SetReadDeadline(time.Now().Add(time.Second))
	m, err := readControl(c)
	if err != nil || m.typ != controlPing {
		t.Fatalf("ping missing: %v %v", m, err)
	}
}
