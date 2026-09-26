package session

import "context"

type FrameKind byte

const (
	FrameH264 FrameKind = iota + 1
	FrameBGRA
)

type Frame struct {
	Data []byte
	Kind FrameKind
}

type EventKind byte

const (
	EventKey EventKind = iota + 1
	EventPointer
	EventText
	EventReleaseAll
	EventSynchronize
)

type Event struct {
	Kind    EventKind
	Payload []byte
}

type Bus struct {
	subs   []chan Frame
	inputs chan Event
	latest *Frame
}

func NewBus(subscribers int) *Bus {
	if subscribers < 1 {
		subscribers = 1
	}
	return &Bus{inputs: make(chan Event, subscribers)}
}

func (b *Bus) Subscribe(ctx context.Context) <-chan Frame {
	out := make(chan Frame, 1)
	if b.latest != nil {
		out <- *b.latest
	}
	b.subs = append(b.subs, out)
	go func() {
		defer close(out)
		<-ctx.Done()
	}()
	return out
}

func (b *Bus) Publish(frame Frame) {
	b.latest = &frame
	for _, sub := range b.subs {
		select {
		case <-sub:
		default:
		}
		select {
		case sub <- frame:
		default:
			select {
			case <-sub:
			default:
			}
			sub <- frame
		}
	}
}

func (b *Bus) Input(event Event) {
	b.inputs <- event
}

func (b *Bus) Inputs() <-chan Event { return b.inputs }
