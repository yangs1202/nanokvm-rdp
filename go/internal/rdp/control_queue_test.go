package rdp

import (
	"testing"
	"time"
)

func TestControlQueueKeepsKeyboardWhenVideoAcksFillTheBuffer(t *testing.T) {
	session := &cgoSession{controls: make(chan controlEvent, controlQueueCapacity)}
	for i := 0; i < cap(session.controls); i++ {
		if !session.enqueueControl(controlEvent{kind: 6, payload: []byte{byte(i)}}) {
			t.Fatalf("pointer event %d was dropped before the queue filled", i)
		}
	}

	release := controlEvent{kind: 5, payload: []byte{0x1d, 0, 1}}
	unicode := controlEvent{kind: 14, payload: []byte("안")}
	if !session.enqueueControl(release) || !session.enqueueControl(unicode) {
		t.Fatal("keyboard release and unicode were dropped while frame ACKs occupied the queue")
	}

	deadline := time.After(time.Second)
	var sawRelease, sawUnicode bool
	for !sawRelease || !sawUnicode {
		select {
		case event := <-session.controls:
			if event.kind == release.kind && string(event.payload) == string(release.payload) {
				sawRelease = true
			}
			if event.kind == unicode.kind && string(event.payload) == string(unicode.payload) {
				sawUnicode = true
			}
		case <-deadline:
			t.Fatalf("release=%v unicode=%v", sawRelease, sawUnicode)
		}
	}
}
