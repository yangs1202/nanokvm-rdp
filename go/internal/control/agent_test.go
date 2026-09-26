package control

import (
	"net"
	"testing"
	"time"
)

func TestAgentHelloStartsStreamAndRoutesInput(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	agent, err := ServeAgent(listener, func() bool { return true })
	if err != nil {
		t.Fatal(err)
	}
	defer agent.Close()

	conn, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	hello := make([]byte, HelloCapsSize)
	hello[8] = CapabilityKeyAck
	if err := Write(conn, TypeHello, hello); err != nil {
		t.Fatal(err)
	}
	start, err := Read(conn)
	if err != nil || start.Type != TypeStartStream {
		t.Fatalf("start = %+v err=%v", start, err)
	}
	if err := agent.Send(TypeKey, []byte{0x1e, 0, 0}); err != nil {
		t.Fatal(err)
	}
	key, err := Read(conn)
	if err != nil || key.Type != TypeKey || key.Payload[0] != 0x1e {
		t.Fatalf("key = %+v err=%v", key, err)
	}
}

func TestAgentHeartbeatTimeout(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	agent, err := ServeAgent(listener, func() bool { return false })
	if err != nil {
		t.Fatal(err)
	}
	agent.heartbeat = 20 * time.Millisecond
	agent.timeout = 50 * time.Millisecond
	defer agent.Close()
	conn, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if err := Write(conn, TypeHello, make([]byte, HelloBaseSize)); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(time.Second)
	conn.SetReadDeadline(deadline)
	var sawPing bool
	for time.Now().Before(deadline) {
		msg, err := Read(conn)
		if err != nil {
			if sawPing {
				return
			}
			t.Fatal(err)
		}
		if msg.Type == TypePing {
			sawPing = true
		}
	}
	t.Fatal("connection stayed open")
}
