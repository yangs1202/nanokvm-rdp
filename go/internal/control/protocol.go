package control

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	Version        = 1
	MaxPayload     = 1024
	HelloBaseSize  = 8
	HelloCapsSize  = 9
	StatsSize      = 16
	KeySize        = 3
	KeyAckSize     = 5
	CapabilityKeyAck byte = 0x01
)

type Type byte

const (
	TypeHello Type = iota + 1
	TypeStartStream
	TypeStopStream
	TypeIDRRequest
	TypeKey
	TypePointerAbs
	TypePointerRel
	TypeWheel
	TypeReleaseAll
	TypePing
	TypePong
	TypeStats
	TypeError
	TypeTextUTF8
	TypeKeyAck
	TypeSynchronize
)

var (
	ErrPayloadTooLarge = errors.New("control payload exceeds 1024 bytes")
	ErrBadVersion      = errors.New("control protocol version mismatch")
)

type Message struct {
	Type    Type
	Payload []byte
	Length  uint16
}

func (m Message) SupportsKeyAck() bool {
	return m.Type == TypeHello && len(m.Payload) == HelloCapsSize && m.Payload[8]&CapabilityKeyAck != 0
}

func Frame(kind Type, payload []byte) ([]byte, error) {
	if len(payload) > MaxPayload {
		return nil, ErrPayloadTooLarge
	}
	frame := make([]byte, 4+len(payload))
	frame[0] = Version
	frame[1] = byte(kind)
	binary.BigEndian.PutUint16(frame[2:4], uint16(len(payload)))
	copy(frame[4:], payload)
	return frame, nil
}

func Read(r io.Reader) (Message, error) {
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return Message{}, err
	}
	if header[0] != Version {
		return Message{}, ErrBadVersion
	}
	length := binary.BigEndian.Uint16(header[2:4])
	if length > MaxPayload {
		return Message{}, ErrPayloadTooLarge
	}
	payload := make([]byte, length)
	if length > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return Message{}, err
		}
	}
	return Message{Type: Type(header[1]), Payload: payload, Length: length}, nil
}

func Write(w io.Writer, kind Type, payload []byte) error {
	frame, err := Frame(kind, payload)
	if err != nil {
		return err
	}
	_, err = w.Write(frame)
	if err != nil {
		return fmt.Errorf("write control frame: %w", err)
	}
	return nil
}

func PutUint16(b []byte, v uint16) { binary.BigEndian.PutUint16(b, v) }
func PutUint32(b []byte, v uint32) { binary.BigEndian.PutUint32(b, v) }
func Uint16(b []byte) uint16       { return binary.BigEndian.Uint16(b) }
func Uint32(b []byte) uint32       { return binary.BigEndian.Uint32(b) }
