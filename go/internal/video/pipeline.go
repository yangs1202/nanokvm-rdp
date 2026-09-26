package video

type Pipeline struct {
	Publish    func([]byte)
	RequestIDR func()
}

func (p *Pipeline) Handle(unit []byte, loss bool) {
	if loss && p.RequestIDR != nil {
		p.RequestIDR()
	}
	if p.Publish != nil && len(unit) > 0 {
		p.Publish(unit)
	}
}

type DirectH264 struct {
	Send       func([]byte) error
	RequestIDR func()
	needIDR    bool
	started    bool
}

func (d *DirectH264) Handle(unit []byte, loss bool) {
	if !d.started {
		d.started = true
		d.needIDR = true
	}
	if loss {
		d.needIDR = true
		if d.RequestIDR != nil {
			d.RequestIDR()
		}
	}
	idr := containsNAL(unit, 5)
	p := containsNAL(unit, 1)
	if d.needIDR && !idr {
		if p && d.RequestIDR != nil {
			d.RequestIDR()
		}
		return
	}
	if idr {
		d.needIDR = false
	}
	if (idr || p) && d.Send != nil {
		_ = d.Send(unit)
	}
}

func containsNAL(data []byte, kind byte) bool {
	for i := 0; i+3 < len(data); i++ {
		start := 0
		if data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 {
			start = 3
		} else if i+4 < len(data) && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1 {
			start = 4
		}
		if start != 0 && i+start < len(data) && data[i+start]&0x1f == kind {
			return true
		}
	}
	return false
}
