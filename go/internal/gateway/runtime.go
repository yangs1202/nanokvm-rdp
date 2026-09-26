package gateway

import (
	"context"
	"fmt"
	"net"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/control"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/session"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/video"
)

type RuntimeConfig struct {
	Control net.Listener
	Video   net.PacketConn
}

type Runtime struct {
	bus   *session.Bus
	agent *control.Agent
	video net.PacketConn
	ctx   context.Context
	stop  context.CancelFunc
}

func NewRuntime(cfg RuntimeConfig) *Runtime {
	ctx, stop := context.WithCancel(context.Background())
	rt := &Runtime{bus: session.NewBus(4), ctx: ctx, stop: stop}
	agent, err := control.ServeAgent(cfg.Control, func() bool { return false })
	if err == nil {
		rt.agent = agent
	}
	rt.video = cfg.Video
	go rt.forwardInputs()
	if rt.video != nil {
		go rt.receiveVideo()
	}
	return rt
}

func (rt *Runtime) receiveVideo() {
	var reassembler video.Reassembler
	buffer := make([]byte, 1500)
	for {
		n, _, err := rt.video.ReadFrom(buffer)
		if err != nil {
			return
		}
		unit, _, err := reassembler.Push(append([]byte(nil), buffer[:n]...))
		if err != nil || unit == nil {
			continue
		}
		rt.bus.Publish(session.Frame{Kind: session.FrameH264, Data: unit})
	}
}

func (rt *Runtime) forwardInputs() {
	for {
		select {
		case <-rt.ctx.Done():
			return
		case event := <-rt.bus.Inputs():
			if rt.agent == nil {
				continue
			}
			kind := control.TypeKey
			switch event.Kind {
			case session.EventText:
				kind = control.TypeTextUTF8
			case session.EventReleaseAll:
				kind = control.TypeReleaseAll
			case session.EventSynchronize:
				kind = control.TypeSynchronize
			case session.EventPointer:
				kind = control.TypePointerAbs
			}
			_ = rt.agent.Send(kind, event.Payload)
		}
	}
}

func (rt *Runtime) Close() {
	rt.stop()
	if rt.agent != nil {
		_ = rt.agent.Close()
	}
	if rt.video != nil {
		_ = rt.video.Close()
	}
}

func Run(ctx context.Context, cfg Config) error {
	controlListener, err := net.Listen("tcp", fmt.Sprintf("%s:%d", cfg.BindAddress, cfg.ControlPort))
	if err != nil {
		return fmt.Errorf("listen control: %w", err)
	}
	video, err := net.ListenPacket("udp", fmt.Sprintf(":%d", cfg.VideoPort))
	if err != nil {
		_ = controlListener.Close()
		return fmt.Errorf("listen video: %w", err)
	}
	rt := NewRuntime(RuntimeConfig{Control: controlListener, Video: video})
	defer rt.Close()
	<-ctx.Done()
	return nil
}
