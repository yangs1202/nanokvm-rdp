package gateway

import (
	"net"
	"testing"
	"time"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/control"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/session"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/video"
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

func TestRuntimePublishesRTPAccessUnit(t *testing.T) {
	videoConn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	rt := NewRuntime(RuntimeConfig{Video: videoConn})
	defer rt.Close()
	sub := rt.bus.Subscribe(rt.ctx)
	sender, err := net.Dial("udp", videoConn.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()
	packetizer := video.Packetizer{Sequence: 1, SSRC: 7, MTU: video.DefaultMTU}
	nal := []byte{0x65, 0x01, 0x02}
	if err := packetizer.Packetize(nal, 1000, true, func(packet []byte) error {
		_, err := sender.Write(packet)
		return err
	}); err != nil {
		t.Fatal(err)
	}
	select {
	case frame := <-sub:
		if len(frame.Data) < 5 || frame.Data[4] != 0x65 {
			t.Fatalf("frame = %x", frame.Data)
		}
	case <-time.After(time.Second):
		t.Fatal("no video frame")
	}
}
