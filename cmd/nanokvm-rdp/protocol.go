package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
)

const (
	protocolVersion   byte = 1
	maxControlPayload      = 1024

	controlHello       byte = 1
	controlStartStream byte = 2
	controlStopStream  byte = 3
	controlIDRRequest  byte = 4
	controlKey         byte = 5
	controlPointerAbs  byte = 6
	controlPointerRel  byte = 7
	controlWheel       byte = 8
	controlReleaseAll  byte = 9
	controlPing        byte = 10
	controlPong        byte = 11
	controlStats       byte = 12
	controlError       byte = 13
	controlTextUTF8    byte = 14
	controlKeyAck      byte = 15
	controlSynchronize byte = 16
)

const (
	helloBaseLength  = 8
	helloCapsLength  = 9
	keyAckCapability = 1
)

type controlMessage struct {
	typ     byte
	payload []byte
}

func writeControl(conn net.Conn, typ byte, payload []byte) error {
	if len(payload) > maxControlPayload {
		return fmt.Errorf("control payload too large: %d", len(payload))
	}
	frame := make([]byte, 4+len(payload))
	frame[0] = protocolVersion
	frame[1] = typ
	binary.BigEndian.PutUint16(frame[2:], uint16(len(payload)))
	copy(frame[4:], payload)
	for len(frame) > 0 {
		n, err := conn.Write(frame)
		if err != nil {
			return fmt.Errorf("write control frame: %w", err)
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		frame = frame[n:]
	}
	return nil
}

func readControl(conn net.Conn) (controlMessage, error) {
	var header [4]byte
	if _, err := io.ReadFull(conn, header[:]); err != nil {
		return controlMessage{}, fmt.Errorf("read control header: %w", err)
	}
	if header[0] != protocolVersion {
		return controlMessage{}, fmt.Errorf("unsupported control version %d", header[0])
	}
	length := int(binary.BigEndian.Uint16(header[2:]))
	if length > maxControlPayload {
		return controlMessage{}, fmt.Errorf("control payload too large: %d", length)
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return controlMessage{}, fmt.Errorf("read control payload: %w", err)
	}
	return controlMessage{typ: header[1], payload: payload}, nil
}

func u16(data []byte) uint16           { return binary.BigEndian.Uint16(data) }
func putU16(data []byte, value uint16) { binary.BigEndian.PutUint16(data, value) }

func putU32(data []byte, value uint32) { binary.BigEndian.PutUint32(data, value) }
