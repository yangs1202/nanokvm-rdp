package main

import (
	"io"
	"log/slog"
	"net"
	"testing"
	"time"
)

func TestLateHelloAfterShutdownDoesNotInstallAgent(t *testing.T) {
	g, err := NewGateway(reviewConfig(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	server, client := net.Pipe()
	defer client.Close()
	done := make(chan struct{})
	go func() { g.handleAgent(server); close(done) }()
	g.Close()
	_ = client.SetDeadline(time.Now().Add(time.Second))
	_ = writeControl(client, controlHello, make([]byte, helloBaseLength))
	g.controlMu.Lock()
	installed := g.agent != nil
	g.controlMu.Unlock()
	if installed {
		t.Fatal("late HELLO installed an agent after shutdown")
	}
	_ = client.Close()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("late HELLO handler did not exit")
	}
}
