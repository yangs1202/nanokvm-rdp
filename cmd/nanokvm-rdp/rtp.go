package main

import (
	"encoding/binary"
	"fmt"
)

type h264AccessUnit struct {
	data     []byte
	keyframe bool
}

type directH264State struct {
	sps     []byte
	pps     []byte
	needIDR bool
}

func annexBNALs(data []byte) [][]byte {
	var nals [][]byte
	for offset := 0; offset < len(data); {
		start := -1
		startLength := 0
		for index := offset; index+3 <= len(data); index++ {
			if data[index] != 0 || data[index+1] != 0 {
				continue
			}
			if data[index+2] == 1 {
				start, startLength = index, 3
				break
			}
			if index+3 < len(data) && data[index+2] == 0 && data[index+3] == 1 {
				start, startLength = index, 4
				break
			}
		}
		if start < 0 {
			break
		}
		nalStart := start + startLength
		nalEnd := len(data)
		for index := nalStart; index+3 <= len(data); index++ {
			if data[index] == 0 && data[index+1] == 0 &&
				(data[index+2] == 1 || (index+3 < len(data) && data[index+2] == 0 && data[index+3] == 1)) {
				nalEnd = index
				break
			}
		}
		if nalStart < nalEnd {
			nals = append(nals, data[nalStart:nalEnd])
		}
		offset = nalEnd
	}
	return nals
}

func annexBNALWithStartCode(nal []byte) []byte {
	return append([]byte{0, 0, 0, 1}, nal...)
}

func (s *directH264State) prepare(unit h264AccessUnit) ([]byte, bool) {
	var hasSPS, hasPPS, hasIDR, hasP bool
	for _, nal := range annexBNALs(unit.data) {
		switch nal[0] & 0x1f {
		case 1:
			hasP = true
		case 5:
			hasIDR = true
		case 7:
			hasSPS = true
			s.sps = annexBNALWithStartCode(nal)
		case 8:
			hasPPS = true
			s.pps = annexBNALWithStartCode(nal)
		}
	}
	if hasIDR || unit.keyframe {
		s.needIDR = false
		prefix := make([]byte, 0, len(s.sps)+len(s.pps)+len(unit.data))
		if !hasSPS {
			prefix = append(prefix, s.sps...)
		}
		if !hasPPS {
			prefix = append(prefix, s.pps...)
		}
		if len(prefix) == 0 {
			return unit.data, true
		}
		prefix = append(prefix, unit.data...)
		return prefix, true
	}
	if s.needIDR || !hasP {
		return nil, false
	}
	return unit.data, true
}

type rtpH264Assembler struct {
	haveSequence bool
	sequence     uint16
	timestamp    uint32
	current      []byte
	keyframe     bool
	lost         bool
	fuActive     bool
	lastLoss     bool
}

const maxH264AccessUnit = 8 * 1024 * 1024

func (a *rtpH264Assembler) push(packet []byte) (h264AccessUnit, bool, error) {
	a.lastLoss = false
	if len(packet) < 12 || packet[0]>>6 != 2 || packet[1]&0x7f != 96 {
		return h264AccessUnit{}, false, fmt.Errorf("invalid RTP packet")
	}
	cc := int(packet[0] & 0x0f)
	headerLength := 12 + 4*cc
	if len(packet) < headerLength {
		return h264AccessUnit{}, false, fmt.Errorf("truncated RTP header")
	}
	if packet[0]&0x10 != 0 {
		if len(packet) < headerLength+4 {
			return h264AccessUnit{}, false, fmt.Errorf("truncated RTP extension")
		}
		extensionWords := int(binary.BigEndian.Uint16(packet[headerLength+2:]))
		headerLength += 4 + extensionWords*4
		if len(packet) < headerLength {
			return h264AccessUnit{}, false, fmt.Errorf("truncated RTP extension body")
		}
	}
	if packet[0]&0x20 != 0 {
		padding := int(packet[len(packet)-1])
		if padding == 0 || padding > len(packet)-headerLength {
			return h264AccessUnit{}, false, fmt.Errorf("invalid RTP padding")
		}
		packet = packet[:len(packet)-padding]
	}
	sequence := binary.BigEndian.Uint16(packet[2:])
	timestamp := binary.BigEndian.Uint32(packet[4:])
	marker := packet[1]&0x80 != 0
	payload := packet[headerLength:]
	if len(payload) == 0 {
		return h264AccessUnit{}, false, fmt.Errorf("empty RTP payload")
	}
	if a.haveSequence && sequence != a.sequence+1 {
		a.current = nil
		a.keyframe = false
		a.fuActive = false
		a.lost = true
		a.lastLoss = true
	}
	a.haveSequence = true
	a.sequence = sequence
	if len(a.current) > 0 && timestamp != a.timestamp {
		// A sender that omitted the marker still gets a bounded flush at the
		// timestamp boundary; the next packet starts a new access unit.
		partial := a.fuActive
		oldLoss := a.lost || partial
		a.lastLoss = a.lastLoss || oldLoss
		unit := h264AccessUnit{data: a.current, keyframe: a.keyframe}
		a.current = nil
		a.keyframe = false
		a.fuActive = false
		a.lost = false
		a.timestamp = timestamp
		if !a.appendPayload(payload) {
			a.current = nil
			a.keyframe = false
			a.fuActive = false
			a.lost = true
			a.lastLoss = true
			return h264AccessUnit{}, false, nil
		}
		if len(a.current) > maxH264AccessUnit {
			a.current = nil
			a.keyframe = false
			a.fuActive = false
			a.lastLoss = true
			return h264AccessUnit{}, false, nil
		}
		if marker {
			if a.fuActive {
				a.current = nil
				a.keyframe = false
				a.fuActive = false
				a.lost = true
				a.lastLoss = true
				return h264AccessUnit{}, false, nil
			}
			newUnit := h264AccessUnit{data: a.current, keyframe: a.keyframe}
			a.current = nil
			a.keyframe = false
			a.fuActive = false
			return newUnit, !a.lost || newUnit.keyframe, nil
		}
		return unit, !oldLoss, nil
	}
	a.timestamp = timestamp
	if !a.appendPayload(payload) {
		a.current = nil
		a.keyframe = false
		a.fuActive = false
		a.lost = true
		a.lastLoss = true
		return h264AccessUnit{}, false, nil
	}
	if len(a.current) > maxH264AccessUnit {
		a.current = nil
		a.keyframe = false
		a.fuActive = false
		a.lost = true
		a.lastLoss = true
		return h264AccessUnit{}, false, nil
	}
	if !marker {
		return h264AccessUnit{}, false, nil
	}
	if a.fuActive {
		a.current = nil
		a.keyframe = false
		a.fuActive = false
		a.lost = true
		a.lastLoss = true
		return h264AccessUnit{}, false, nil
	}
	unit := h264AccessUnit{data: a.current, keyframe: a.keyframe}
	a.current = nil
	a.keyframe = false
	a.fuActive = false
	lost := a.lost
	a.lost = false
	if lost {
		a.lastLoss = true
	}
	return unit, !lost || unit.keyframe, nil
}

func (a *rtpH264Assembler) appendPayload(payload []byte) bool {
	nalType := payload[0] & 0x1f
	switch {
	case nalType >= 1 && nalType <= 23:
		a.current = append(a.current, 0, 0, 0, 1)
		a.current = append(a.current, payload...)
		if nalType == 5 {
			a.keyframe = true
		}
		return true
	case nalType == 24:
		payload = payload[1:]
		var nals [][]byte
		for len(payload) >= 2 {
			length := int(binary.BigEndian.Uint16(payload))
			if length <= 0 || len(payload) < 2+length {
				return false
			}
			nals = append(nals, payload[2:2+length])
			payload = payload[2+length:]
		}
		if len(payload) != 0 {
			return false
		}
		for _, nal := range nals {
			a.current = append(a.current, 0, 0, 0, 1)
			a.current = append(a.current, nal...)
			if nal[0]&0x1f == 5 {
				a.keyframe = true
			}
		}
		return true
	case nalType == 28 && len(payload) >= 2:
		start := payload[1]&0x80 != 0
		end := payload[1]&0x40 != 0
		fragmentType := payload[1] & 0x1f
		if start {
			a.current = append(a.current, 0, 0, 0, 1, (payload[0]&0xe0)|fragmentType)
			a.fuActive = true
			if fragmentType == 5 {
				a.keyframe = true
			}
		}
		if !a.fuActive {
			return false
		}
		a.current = append(a.current, payload[2:]...)
		if end {
			a.fuActive = false
		}
		return true
	}
	return false
}
