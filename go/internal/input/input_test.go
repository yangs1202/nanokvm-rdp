package input

import "testing"

func TestMapAltCommandSwap(t *testing.T) {
	code, extended := MapScancode(0x38, false, true)
	if code != 0x5b || !extended {
		t.Fatalf("left alt -> %+v %v", code, extended)
	}
	code, extended = MapScancode(0x5b, true, true)
	if code != 0x38 || extended {
		t.Fatalf("left gui -> %+v %v", code, extended)
	}
	code, extended = MapScancode(0x1c, false, true)
	if code != 0x1c || extended {
		t.Fatalf("enter changed: %x %v", code, extended)
	}
}

func TestPointerButtonsAndClamp(t *testing.T) {
	buttons := PointerButtons(0, PointerDown|PointerButton1)
	if buttons != 0x01 {
		t.Fatalf("down = %#x", buttons)
	}
	buttons = PointerButtons(buttons, PointerButton1)
	if buttons != 0 {
		t.Fatalf("up = %#x", buttons)
	}
	if ClampAbsolute(2000, 1920) != 1919 || ClampAbsolute(3, 0) != 0 {
		t.Fatal("clamp")
	}
}

func TestRelativeHorizontalWheelRewrite(t *testing.T) {
	flags := NormalizeRelativeWheel(PointerWheel|0x0078, -4, 0)
	if flags&PointerHWheel == 0 || flags&PointerWheelNegative == 0 {
		t.Fatalf("flags = %#x", flags)
	}
	if flags&0x00ff != 0x0078 {
		t.Fatalf("rotation = %#x", flags)
	}
}

func TestUnicodeUTF8(t *testing.T) {
	got, err := UnicodePayload(0xac00)
	if err != nil || len(got) != 3 || got[0] != 0xea {
		t.Fatalf("payload = %x err=%v", got, err)
	}
	if _, err := UnicodePayload(0xd800); err == nil {
		t.Fatal("surrogate accepted")
	}
}
