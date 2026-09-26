package control

import (
	"bytes"
	"errors"
	"io"
	"testing"
)

func TestFrameRoundTrip(t *testing.T) {
	payload := []byte{0x01, 0x02, 0x03}
	frame, err := Frame(TypeKey, payload)
	if err != nil {
		t.Fatal(err)
	}
	got, err := Read(bytes.NewReader(frame))
	if err != nil {
		t.Fatal(err)
	}
	if got.Type != TypeKey || got.Length != 3 || !bytes.Equal(got.Payload, payload) {
		t.Fatalf("message = %+v", got)
	}
}

func TestFrameRejectsOversizedPayload(t *testing.T) {
	_, err := Frame(TypeError, make([]byte, MaxPayload+1))
	if !errors.Is(err, ErrPayloadTooLarge) {
		t.Fatalf("err = %v", err)
	}
}

func TestReadRejectsBadVersionAndShortBody(t *testing.T) {
	if _, err := Read(bytes.NewReader([]byte{2, byte(TypePing), 0, 0})); !errors.Is(err, ErrBadVersion) {
		t.Fatalf("version err = %v", err)
	}
	_, err := Read(bytes.NewReader([]byte{Version, byte(TypeKey), 0, 2, 0x01}))
	if !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, io.EOF) {
		t.Fatalf("short body err = %v", err)
	}
}

func TestHelloCapabilities(t *testing.T) {
	msg := Message{Type: TypeHello, Payload: []byte{0, 0, 0, 0, 0, 0, 0, 0, CapabilityKeyAck}}
	if !msg.SupportsKeyAck() {
		t.Fatal("expected key ack capability")
	}
	msg.Payload = msg.Payload[:HelloBaseSize]
	if msg.SupportsKeyAck() {
		t.Fatal("base hello has no capabilities")
	}
}

func TestPutUint(t *testing.T) {
	var b [4]byte
	PutUint16(b[:2], 0x1234)
	PutUint32(b[:], 0x01020304)
	if Uint16(b[:2]) != 0x0102 || Uint32(b[:]) != 0x01020304 {
		t.Fatalf("bytes = %x", b)
	}
}
