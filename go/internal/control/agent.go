package control

import (
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"
)

type Agent struct {
	listener   net.Listener
	wantStream func() bool
	heartbeat  time.Duration
	timeout    time.Duration
	mu         sync.Mutex
	conn       net.Conn
	done       chan struct{}
}

func ServeAgent(listener net.Listener, wantStream func() bool) (*Agent, error) {
	if listener == nil {
		return nil, errors.New("control listener is nil")
	}
	agent := &Agent{
		listener:   listener,
		wantStream: wantStream,
		heartbeat:  time.Second,
		timeout:    5 * time.Second,
		done:       make(chan struct{}),
	}
	go agent.accept()
	return agent, nil
}

func (a *Agent) Close() error {
	select {
	case <-a.done:
	default:
		close(a.done)
	}
	return a.listener.Close()
}

func (a *Agent) Send(kind Type, payload []byte) error {
	a.mu.Lock()
	conn := a.conn
	a.mu.Unlock()
	if conn == nil {
		return errors.New("agent is not connected")
	}
	if err := Write(conn, kind, payload); err != nil {
		_ = conn.Close()
		return err
	}
	return nil
}

func (a *Agent) accept() {
	for {
		conn, err := a.listener.Accept()
		if err != nil {
			return
		}
		go a.serve(conn)
	}
}

func (a *Agent) serve(conn net.Conn) {
	defer conn.Close()
	hello, err := Read(conn)
	if err != nil || hello.Type != TypeHello || (len(hello.Payload) != HelloBaseSize && len(hello.Payload) != HelloCapsSize) {
		return
	}
	a.mu.Lock()
	if a.conn != nil {
		_ = a.conn.Close()
	}
	a.conn = conn
	start := a.wantStream != nil && a.wantStream()
	a.mu.Unlock()
	if start {
		if err := Write(conn, TypeStartStream, nil); err != nil {
			return
		}
	}
	activity := time.Now()
	nextPing := time.Now().Add(a.heartbeat)
	for {
		_ = conn.SetReadDeadline(nextPing)
		msg, err := Read(conn)
		if err != nil {
			if time.Since(activity) > a.timeout {
				return
			}
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				if err := Write(conn, TypePing, nil); err != nil {
					return
				}
				nextPing = time.Now().Add(a.heartbeat)
				continue
			}
			if errors.Is(err, io.EOF) {
				return
			}
			return
		}
		activity = time.Now()
		if msg.Type == TypePing {
			if err := Write(conn, TypePong, nil); err != nil {
				return
			}
		}
	}
}

func (a *Agent) String() string { return fmt.Sprintf("agent timeout=%s", a.timeout) }
