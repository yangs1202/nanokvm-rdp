package gateway

import (
	"context"
	"net"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/control"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/session"
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
	return rt
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
