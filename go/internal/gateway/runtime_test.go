package gateway

import (
	"net"
	"testing"
	"time"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/control"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/session"
)

func TestRuntimeForwardsControlInputAndVideo(t *testing.T) {
	controlListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	video, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	rt := NewRuntime(RuntimeConfig{Control: controlListener, Video: video})
	defer rt.Close()

	agent, err := net.Dial("tcp", controlListener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer agent.Close()
	if err := control.Write(agent, control.TypeHello, make([]byte, control.HelloBaseSize)); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if err := rt.agent.Send(control.TypePing, nil); err == nil {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	rt.bus.Input(session.Event{Kind: session.EventKey, Payload: []byte{0x1e, 0, 0}})
	_ = agent.SetReadDeadline(deadline)
	for {
		msg, err := control.Read(agent)
		if err != nil {
			t.Fatal(err)
		}
		if msg.Type == control.TypePing {
			continue
		}
		if msg.Type != control.TypeKey || msg.Payload[0] != 0x1e {
			t.Fatalf("input = %+v", msg)
		}
		return
	}
}
