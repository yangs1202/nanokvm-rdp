package session

import (
	"context"
	"testing"
	"time"
)

func TestBusLatestFrameDoesNotBlockPublisher(t *testing.T) {
	bus := NewBus(1)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	sub := bus.Subscribe(ctx)
	slow, slowCancel := context.WithCancel(context.Background())
	defer slowCancel()
	_ = bus.Subscribe(slow)
	for i := byte(1); i <= 3; i++ {
		bus.Publish(Frame{Data: []byte{i}, Kind: FrameH264})
	}
	select {
	case frame := <-sub:
		if frame.Data[0] != 3 {
			t.Fatalf("frame = %v", frame.Data)
		}
	case <-time.After(time.Second):
		t.Fatal("no frame")
	}
}

func TestBusInputFanInPreservesOrder(t *testing.T) {
	bus := NewBus(1)
	go func() {
		bus.Input(Event{Kind: EventKey, Payload: []byte{1}})
		bus.Input(Event{Kind: EventKey, Payload: []byte{2}})
	}()
	var got []byte
	deadline := time.After(time.Second)
	for len(got) < 2 {
		select {
		case event := <-bus.Inputs():
			got = append(got, event.Payload...)
		case <-deadline:
			t.Fatal("timeout")
		}
	}
	if got[0] != 1 || got[1] != 2 {
		t.Fatalf("order = %v", got)
	}
}

func TestBusReplaysLatestFrameToNewSubscriber(t *testing.T) {
	bus := NewBus(1)
	bus.Publish(Frame{Data: []byte{9}, Kind: FrameH264})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	sub := bus.Subscribe(ctx)
	select {
	case frame := <-sub:
		if frame.Data[0] != 9 {
			t.Fatalf("frame = %v", frame.Data)
		}
	case <-time.After(time.Second):
		t.Fatal("latest frame was not replayed")
	}
}
