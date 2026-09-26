package video

import (
	"bytes"
	"testing"
)

func TestPipelinePublishesAccessUnitAndRequestsIDROnLoss(t *testing.T) {
	var published [][]byte
	var idr int
	p := &Pipeline{
		Publish: func(unit []byte) { published = append(published, unit) },
		RequestIDR: func() { idr++ },
	}
	unit := append([]byte{0, 0, 0, 1, 0x65}, bytes.Repeat([]byte{1}, 8)...)
	p.Handle(unit, false)
	p.Handle(unit, true)
	if len(published) != 2 || idr != 1 {
		t.Fatalf("published=%d idr=%d", len(published), idr)
	}
}

func TestDirectH264NeedsIDRBeforePFrame(t *testing.T) {
	var sent [][]byte
	var requests int
	d := &DirectH264{
		Send: func(data []byte) error { sent = append(sent, data); return nil },
		RequestIDR: func() { requests++ },
	}
	p := []byte{0, 0, 0, 1, 0x41, 0x01}
	d.Handle(p, false)
	if len(sent) != 0 || requests != 1 {
		t.Fatalf("p before idr sent=%d requests=%d", len(sent), requests)
	}
	idr := []byte{0, 0, 0, 1, 0x65, 0x02}
	d.Handle(idr, false)
	d.Handle(p, false)
	if len(sent) != 2 {
		t.Fatalf("sent=%d", len(sent))
	}
}
