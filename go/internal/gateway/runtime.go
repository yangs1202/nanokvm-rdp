package gateway

import (
	"context"
	"fmt"
	"net"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/control"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/rdp"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/session"
	"github.com/yangs1202/nanokvm-rdp/go/go/internal/video"
)

type RuntimeConfig struct {
	Control net.Listener
	Video   net.PacketConn
	RDP     rdp.Session
	Inputs  <-chan rdp.Input
}

type Runtime struct {
	bus    *session.Bus
	agent  *control.Agent
	video  net.PacketConn
	rdp    rdp.Session
	inputs <-chan rdp.Input
	ctx    context.Context
	stop   context.CancelFunc
}

func NewRuntime(cfg RuntimeConfig) *Runtime {
	ctx, stop := context.WithCancel(context.Background())
	rt := &Runtime{bus: session.NewBus(4), ctx: ctx, stop: stop}
	agent, err := control.ServeAgent(cfg.Control, func() bool { return false })
	if err == nil {
		rt.agent = agent
	}
	rt.video = cfg.Video
	rt.rdp = cfg.RDP
	rt.inputs = cfg.Inputs
	go rt.forwardInputs()
	go rt.forwardFrames()
	go rt.forwardSessionControls()
	if rt.video != nil {
		go rt.receiveVideo()
	}
	return rt
}

func (rt *Runtime) forwardSessionControls() {
	if rt.rdp == nil {
		return
	}
	for {
		select {
		case <-rt.ctx.Done():
			return
		case event, ok := <-rt.rdp.Controls():
			if !ok || rt.agent == nil {
				return
			}
			_ = rt.agent.Send(control.Type(event.Type), event.Payload)
		}
	}
}

func (rt *Runtime) forwardFrames() {
	frames := rt.bus.Subscribe(rt.ctx)
	for frame := range frames {
		if rt.rdp == nil {
			continue
		}
		rdpFrame := rdp.Frame{Kind: rdp.FrameH264, H264: frame.Data}
		if frame.Kind == session.FrameBGRA {
			rdpFrame = rdp.Frame{Kind: rdp.FrameBGRA, BGRA: frame.Data}
		}
		_ = rt.rdp.Submit(rdpFrame)
	}
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
	return run(ctx, cfg, true)
}

func run(ctx context.Context, cfg Config, startRDP bool) error {
	var rdpSession rdp.Session
	inputs := make(chan rdp.Input, 16)
	if startRDP {
		var err error
		rdpSession, err = rdp.Start(rdp.Config{
			BindAddress:    cfg.BindAddress,
			Port:           cfg.RDPPort,
			Certificate:    cfg.Certificate,
			PrivateKey:     cfg.PrivateKey,
			Width:          cfg.Width,
			Height:         cfg.Height,
			DirectGFX:      cfg.DirectGFX,
			SwapAltCommand: cfg.SwapAltCommand,
		}, inputs)
		if err != nil {
			return fmt.Errorf("start rdp: %w", err)
		}
		defer rdpSession.Close()
	}
	controlListener, err := net.Listen("tcp", fmt.Sprintf("%s:%d", cfg.BindAddress, cfg.ControlPort))
	if err != nil {
		return fmt.Errorf("listen control: %w", err)
	}
	video, err := net.ListenPacket("udp", fmt.Sprintf(":%d", cfg.VideoPort))
	if err != nil {
		_ = controlListener.Close()
		return fmt.Errorf("listen video: %w", err)
	}
	rt := NewRuntime(RuntimeConfig{Control: controlListener, Video: video, RDP: rdpSession, Inputs: inputs})
	defer rt.Close()
	go rt.forwardRDPInputs()
	<-ctx.Done()
	return nil
}

func (rt *Runtime) forwardRDPInputs() {
	if rt.inputs == nil {
		return
	}
	for {
		select {
		case <-rt.ctx.Done():
			return
		case input := <-rt.inputs:
			kind := session.EventKey
			payload := []byte{byte(input.Code), 0, 0}
			if input.Flags&0x8000 != 0 {
				payload[2] = 1
			}
			if input.Kind == rdp.InputMouse {
				kind = session.EventPointer
				payload = []byte{byte(input.X), byte(input.X >> 8), byte(input.Y), byte(input.Y >> 8)}
			}
			rt.bus.Input(session.Event{Kind: kind, Payload: payload})
		}
	}
}
