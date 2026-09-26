package video

import (
	"bytes"
	"testing"
)

func TestReassemblerSingleAndFragmentedAccessUnit(t *testing.T) {
	var r Reassembler
	sps := []byte{0x67, 0x64, 0x00, 0x29}
	idr := bytes.Repeat([]byte{0x65}, 1300)
	idr[0] = 0x65
	packets := append(packetize(t, sps, 1, 3000, false), packetize(t, idr, 2, 3000, true)...)
	var got []byte
	var losses int
	for _, packet := range packets {
		unit, loss, err := r.Push(packet)
		if err != nil {
			t.Fatal(err)
		}
		if loss {
			losses++
		}
		if unit != nil {
			got = unit
		}
	}
	if losses != 0 || got == nil {
		t.Fatalf("losses=%d got=%d", losses, len(got))
	}
	start := []byte{0, 0, 0, 1}
	if !bytes.HasPrefix(got, start) || !bytes.Equal(got[4:8], sps) {
		t.Fatalf("prefix = %x", got[:12])
	}
	if !bytes.Equal(got[8:12], start) || !bytes.Equal(got[12:], idr) {
		t.Fatalf("idr mismatch len=%d", len(got))
	}
}

func TestReassemblerLossDropsPartialAccessUnit(t *testing.T) {
	var r Reassembler
	first := packetize(t, []byte{0x67, 0x01}, 1, 1, false)[0]
	if _, loss, err := r.Push(first); err != nil || loss {
		t.Fatalf("loss=%v err=%v", loss, err)
	}
	gap := packetize(t, []byte{0x41, 0x02}, 3, 2, true)[0]
	unit, loss, err := r.Push(gap)
	if err != nil || !loss || unit == nil {
		t.Fatalf("unit=%d loss=%v err=%v", len(unit), loss, err)
	}
	if !bytes.Equal(unit[4:], []byte{0x41, 0x02}) {
		t.Fatalf("recovered unit = %x", unit)
	}
}

func packetize(t *testing.T, nal []byte, seq, ts uint32, marker bool) [][]byte {
	t.Helper()
	p := Packetizer{Sequence: uint16(seq), SSRC: 42, MTU: DefaultMTU}
	var packets [][]byte
	if err := p.Packetize(nal, ts, marker, func(packet []byte) error {
		packets = append(packets, append([]byte(nil), packet...))
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	return packets
}
