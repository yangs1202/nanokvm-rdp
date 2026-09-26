package main

import (
	"context"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"
)

func TestMediaLoopStopsDuringContinuousRTP(t *testing.T) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()

	ctx, cancel := context.WithCancel(context.Background())
	g := &Gateway{log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	g.mediaWG.Add(1)
	done := make(chan struct{})
	go func() {
		g.mediaLoop(ctx, conn)
		close(done)
	}()

	sender, err := net.DialUDP("udp", nil, conn.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()
	packet := []byte{0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0x65, 0x88}
	stopSender := make(chan struct{})
	go func() {
		ticker := time.NewTicker(time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-stopSender:
				return
			case <-ticker.C:
				_, _ = sender.Write(packet)
			}
		}
	}()
	defer close(stopSender)

	time.Sleep(20 * time.Millisecond)
	cancel()
	select {
	case <-done:
	case <-time.After(500 * time.Millisecond):
		t.Fatal("media loop did not stop while RTP traffic continued")
	}
}
