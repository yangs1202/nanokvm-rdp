package rdp

import "testing"

func TestBridgeBindsWithoutCertificate(t *testing.T) {
	session, err := Start(Config{BindAddress: "127.0.0.1", Port: 0, Width: 64, Height: 64}, nil)
	if err == nil {
		_ = session.Close()
		t.Fatal("port 0 should not start a listener")
	}
}

func TestFakeSessionSubmitsBitmapAndInput(t *testing.T) {
	var frames int
	var keys int
	s := &fakeSession{
		submit:  func(Frame) error { frames++; return nil },
		onInput: func(Input) { keys++ },
	}
	if err := s.Submit(Frame{Width: 2, Height: 1, BGRA: []byte{1, 2, 3, 4, 5, 6, 7, 8}}); err != nil {
		t.Fatal(err)
	}
	s.emit(Input{Kind: InputKey, Code: 0x1e})
	if frames != 1 || keys != 1 {
		t.Fatalf("frames=%d keys=%d", frames, keys)
	}
}

type fakeSession struct {
	submit  func(Frame) error
	onInput func(Input)
}

func (f *fakeSession) Submit(frame Frame) error { return f.submit(frame) }
func (f *fakeSession) emit(input Input) {
	if f.onInput != nil {
		f.onInput(input)
	}
}
