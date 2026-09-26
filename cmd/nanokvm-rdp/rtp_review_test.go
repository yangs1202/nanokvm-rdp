package main

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func packet(seq uint16, stamp uint32, marker bool, payload ...byte) []byte {
	b := make([]byte, 12)
	b[0], b[1] = 0x80, 96
	if marker {
		b[1] |= 0x80
	}
	binary.BigEndian.PutUint16(b[2:], seq)
	binary.BigEndian.PutUint32(b[4:], stamp)
	return append(b, payload...)
}

func TestReviewSTAPTwoNALs(t *testing.T) {
	var a rtpH264Assembler
	u, ok, err := a.push(packet(1, 100, true, 24, 0, 2, 0x67, 0xaa, 0, 2, 0x68, 0xbb))
	want := []byte{0, 0, 0, 1, 0x67, 0xaa, 0, 0, 0, 1, 0x68, 0xbb}
	if err != nil || !ok || !bytes.Equal(u.data, want) {
		t.Fatalf("two NALs not preserved: data=%x ok=%v err=%v", u.data, ok, err)
	}
}

func TestReviewIncompleteFUNotDelivered(t *testing.T) {
	var a rtpH264Assembler
	_, _, _ = a.push(packet(1, 100, false, 28, 0x85, 0xaa))
	u, ok, _ := a.push(packet(2, 200, true, 0x65, 0xbb))
	if ok && bytes.Equal(u.data, []byte{0, 0, 0, 1, 5, 0xaa}) {
		t.Fatal("delivered truncated IDR from unfinished FU-A")
	}
}

func TestReviewIncompleteFUMarkerNotDelivered(t *testing.T) {
	var a rtpH264Assembler
	u, ok, err := a.push(packet(1, 100, true, 28, 0x85, 0xaa))
	if err != nil || ok || len(u.data) != 0 {
		t.Fatalf("incomplete FU-A was delivered: data=%x ok=%v err=%v", u.data, ok, err)
	}
}

func TestReviewCompleteIDRAfterPartialIsPreserved(t *testing.T) {
	var a rtpH264Assembler
	_, _, _ = a.push(packet(1, 100, false, 28, 0x85, 0xaa))
	u, ok, err := a.push(packet(2, 200, true, 0x65, 0xbb))
	want := []byte{0, 0, 0, 1, 0x65, 0xbb}
	if err != nil || !ok || !bytes.Equal(u.data, want) {
		t.Fatalf("complete IDR after partial was discarded: data=%x ok=%v err=%v", u.data, ok, err)
	}
}

func TestReviewMalformedSTAPNotPartiallyDelivered(t *testing.T) {
	var a rtpH264Assembler
	u, ok, err := a.push(packet(1, 100, true, 24, 0, 2, 0x67, 0xaa, 0, 4, 0x68))
	if err != nil || ok || len(u.data) != 0 {
		t.Fatalf("malformed STAP-A was partially delivered: data=%x ok=%v err=%v", u.data, ok, err)
	}
}

func TestReviewPaddingExcluded(t *testing.T) {
	var a rtpH264Assembler
	b := packet(1, 100, true, 0x65, 0xaa, 0, 2)
	b[0] |= 0x20
	u, ok, err := a.push(b)
	want := []byte{0, 0, 0, 1, 0x65, 0xaa}
	if err != nil || !ok || !bytes.Equal(u.data, want) {
		t.Fatalf("padding leaked into H264: %x ok=%v err=%v", u.data, ok, err)
	}
}

func TestDirectH264SplitParameterSetsArePrependedToIDR(t *testing.T) {
	var assembler rtpH264Assembler
	var state = directH264State{needIDR: true}
	feed := func(sequence uint16, timestamp uint32, nal byte) ([]byte, bool) {
		unit, ready, err := assembler.push(packet(sequence, timestamp, true, nal, 0x11))
		if err != nil || !ready {
			t.Fatalf("split parameter RTP unit was not assembled: ready=%v err=%v", ready, err)
		}
		return state.prepare(unit)
	}

	if payload, send := feed(1, 100, 0x67); send || payload != nil {
		t.Fatal("SPS-only access unit was sent")
	}
	if payload, send := feed(2, 200, 0x68); send || payload != nil {
		t.Fatal("PPS-only access unit was sent")
	}
	payload, send := feed(3, 300, 0x65)
	want := []byte{0, 0, 0, 1, 0x67, 0x11, 0, 0, 0, 1, 0x68, 0x11, 0, 0, 0, 1, 0x65, 0x11}
	if !send || !bytes.Equal(payload, want) {
		t.Fatalf("split parameter sets were not prepended: %x", payload)
	}
}

func TestDirectH264SuppressesPFramesUntilRecoveryIDR(t *testing.T) {
	var state = directH264State{needIDR: true, sps: []byte{0, 0, 0, 1, 0x67, 0x11}, pps: []byte{0, 0, 0, 1, 0x68, 0x22}}
	pframe := []byte{0, 0, 0, 1, 0x41, 0x44}
	idr := []byte{0, 0, 0, 1, 0x65, 0x55}
	if payload, send := state.prepare(h264AccessUnit{data: pframe}); send || payload != nil {
		t.Fatal("P-frame escaped before recovery IDR")
	}
	payload, send := state.prepare(h264AccessUnit{data: idr, keyframe: true})
	if !send || !bytes.Contains(payload, state.sps) || !bytes.Contains(payload, state.pps) {
		t.Fatalf("recovery IDR did not include cached parameter sets: %x", payload)
	}
	if payload, send := state.prepare(h264AccessUnit{data: pframe}); !send || !bytes.Equal(payload, pframe) {
		t.Fatal("P-frame was not released after recovery IDR")
	}
}
