package video

import (
	"encoding/binary"
	"errors"
)

const (
	DefaultMTU  = 1200
	payloadType = 96
	headerSize  = 12
	maxNAL      = 2 * 1024 * 1024
)

var ErrNALTooLarge = errors.New("h264 nal exceeds limit")

type Packetizer struct {
	Sequence uint16
	SSRC     uint32
	MTU      uint16
}

func (p *Packetizer) Packetize(nal []byte, timestamp uint32, marker bool, emit func([]byte) error) error {
	if len(nal) == 0 || len(nal) > maxNAL {
		return ErrNALTooLarge
	}
	mtu := int(p.MTU)
	if mtu < headerSize+3 || mtu > DefaultMTU {
		mtu = DefaultMTU
	}
	limit := mtu - headerSize
	if len(nal) <= limit {
		return p.emit(nal, marker, timestamp, emit)
	}
	fragmentLimit := limit - 2
	for offset := 1; offset < len(nal); {
		n := len(nal) - offset
		if n > fragmentLimit {
			n = fragmentLimit
		}
		payload := make([]byte, 2+n)
		payload[0] = (nal[0] & 0xe0) | 28
		payload[1] = nal[0] & 0x1f
		if offset == 1 {
			payload[1] |= 0x80
		}
		if offset+n == len(nal) {
			payload[1] |= 0x40
		}
		copy(payload[2:], nal[offset:offset+n])
		if err := p.emit(payload, marker && offset+n == len(nal), timestamp, emit); err != nil {
			return err
		}
		offset += n
	}
	return nil
}

func (p *Packetizer) emit(payload []byte, marker bool, timestamp uint32, emit func([]byte) error) error {
	packet := make([]byte, headerSize+len(payload))
	packet[0] = 0x80
	packet[1] = payloadType
	if marker {
		packet[1] |= 0x80
	}
	binary.BigEndian.PutUint16(packet[2:4], p.Sequence)
	p.Sequence++
	binary.BigEndian.PutUint32(packet[4:8], timestamp)
	binary.BigEndian.PutUint32(packet[8:12], p.SSRC)
	copy(packet[headerSize:], payload)
	return emit(packet)
}

type Reassembler struct {
	expected   uint16
	haveSeq    bool
	assembling bool
	nalHeader  byte
	timestamp  uint32
	buffer     []byte
	unit       []byte
	unitStamp  uint32
	haveUnit   bool
}

func (r *Reassembler) Push(packet []byte) (unit []byte, loss bool, err error) {
	if len(packet) <= headerSize || packet[0]>>6 != 2 {
		return nil, false, nil
	}
	seq := binary.BigEndian.Uint16(packet[2:4])
	if r.haveSeq && seq != r.expected {
		loss = true
		r.assembling = false
		r.buffer = r.buffer[:0]
		r.unit = nil
		r.haveUnit = false
	}
	r.haveSeq = true
	r.expected = seq + 1
	timestamp := binary.BigEndian.Uint32(packet[4:8])
	marker := packet[1]&0x80 != 0
	payload := packet[headerSize:]
	kind := payload[0] & 0x1f
	switch {
	case kind >= 1 && kind <= 23:
		if err := r.save(payload, timestamp); err != nil {
			return nil, loss, err
		}
	case kind == 28 && len(payload) >= 3:
		if err := r.fragment(payload, timestamp, marker); err != nil {
			return nil, loss, err
		}
	default:
		return nil, loss, nil
	}
	if !marker || !r.haveUnit {
		return nil, loss, nil
	}
	out := append([]byte(nil), r.unit...)
	r.unit = nil
	r.haveUnit = false
	return out, loss, nil
}

func (r *Reassembler) fragment(payload []byte, timestamp uint32, marker bool) error {
	start := payload[1]&0x80 != 0
	end := payload[1]&0x40 != 0
	if start {
		r.assembling = true
		r.timestamp = timestamp
		r.buffer = r.buffer[:0]
		r.nalHeader = (payload[0] & 0xe0) | (payload[1] & 0x1f)
		r.buffer = append(r.buffer, r.nalHeader)
	}
	if !r.assembling || timestamp != r.timestamp {
		return nil
	}
	r.buffer = append(r.buffer, payload[2:]...)
	if !end {
		return nil
	}
	err := r.save(r.buffer, timestamp)
	r.assembling = false
	r.buffer = r.buffer[:0]
	if marker {
		return err
	}
	return err
}

func (r *Reassembler) save(nal []byte, timestamp uint32) error {
	if !r.haveUnit || r.unitStamp != timestamp {
		r.unit = r.unit[:0]
		r.unitStamp = timestamp
		r.haveUnit = true
	}
	if len(r.unit)+4+len(nal) > maxNAL {
		return ErrNALTooLarge
	}
	r.unit = append(r.unit, 0, 0, 0, 1)
	r.unit = append(r.unit, nal...)
	return nil
}
