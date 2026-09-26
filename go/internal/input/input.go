package input

import "fmt"

const (
	KeyboardRelease      uint16 = 0x8000
	KeyboardExtended     uint16 = 0x0100
	PointerWheel         uint16 = 0x0200
	PointerWheelNegative uint16 = 0x0100
	PointerHWheel        uint16 = 0x0400
	PointerDown          uint16 = 0x8000
	PointerButton1       uint16 = 0x1000
	PointerButton2       uint16 = 0x2000
	PointerButton3       uint16 = 0x4000
	PointerXButton1      uint16 = 0x0001
	PointerXButton2      uint16 = 0x0002
	WheelRotationMask    uint16 = 0x01FF
)

func MapScancode(code byte, extended, swap bool) (byte, bool) {
	if !swap {
		return code, extended
	}
	switch {
	case code == 0x38:
		if extended {
			return 0x5c, true
		}
		return 0x5b, true
	case extended && code == 0x5b:
		return 0x38, false
	case extended && code == 0x5c:
		return 0x38, true
	default:
		return code, extended
	}
}

func PointerButtons(buttons byte, flags uint16) byte {
	bits := []uint16{PointerButton1, PointerButton2, PointerButton3, PointerXButton1, PointerXButton2}
	for i, bit := range bits {
		if flags&bit == 0 {
			continue
		}
		mask := byte(1 << i)
		if flags&PointerDown != 0 {
			buttons |= mask
		} else {
			buttons &^= mask
		}
	}
	return buttons
}

func ClampAbsolute(value, dimension uint16) uint16 {
	if dimension == 0 {
		return 0
	}
	if value < dimension {
		return value
	}
	return dimension - 1
}

func ExtendedPointerFlags(flags uint16) uint16 {
	return flags & (PointerDown | PointerXButton1 | PointerXButton2)
}

func NormalizeRelativeWheel(flags uint16, dx, dy int16) uint16 {
	vertical := flags&PointerWheel != 0
	horizontal := flags&PointerHWheel != 0
	if !horizontal && vertical && dx != 0 && dy == 0 {
		rotation := flags & WheelRotationMask
		negative := flags&PointerWheelNegative != 0 || dx < 0
		flags &^= PointerWheel | WheelRotationMask | PointerWheelNegative
		flags |= PointerHWheel | rotation
		if negative {
			flags |= PointerWheelNegative
		}
	}
	return flags
}

func UnicodePayload(code uint16) ([]byte, error) {
	if code == 0 || (code >= 0xd800 && code <= 0xdfff) {
		return nil, fmt.Errorf("unsupported unicode code unit U+%04X", code)
	}
	switch {
	case code <= 0x7f:
		return []byte{byte(code)}, nil
	case code <= 0x7ff:
		return []byte{0xc0 | byte(code>>6), 0x80 | byte(code&0x3f)}, nil
	default:
		return []byte{0xe0 | byte(code>>12), 0x80 | byte((code>>6)&0x3f), 0x80 | byte(code&0x3f)}, nil
	}
}
