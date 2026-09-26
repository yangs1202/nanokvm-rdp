package main

import (
	"net"
	"testing"
	"time"
)

func TestWindowsAppHorizontalWheel(t *testing.T) {
	for _, tc := range []struct {
		name string
		dx   uint16
		want uint16
	}{
		{name: "right", dx: 1, want: 0x0478},
		{name: "left", dx: 0xffff, want: 0x0578},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server, client := net.Pipe()
			defer server.Close()
			defer client.Close()
			_ = client.SetDeadline(time.Now().Add(time.Second))
			g := &Gateway{agent: &agentConnection{conn: server}}
			payload := make([]byte, 6)
			putU16(payload, tc.dx)
			putU16(payload[4:], 0x0278)
			done := make(chan bool, 1)
			go func() { done <- g.forwardRelative(payload) }()
			message, err := readControl(client)
			if err != nil {
				t.Fatal(err)
			}
			if !<-done {
				t.Fatal("forward failed")
			}
			if message.typ != controlWheel || len(message.payload) != 2 || u16(message.payload) != tc.want {
				t.Fatalf("horizontal scroll flags %04x; got type=%d payload=%x", tc.want, message.typ, message.payload)
			}
		})
	}
}
