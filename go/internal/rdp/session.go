package rdp

// Session is the FreeRDP boundary. The production implementation lives in
// session_cgo.go and is compiled only when cgo and FreeRDP are available.
type Session interface {
	Submit(Frame) error
	Close() error
	Controls() <-chan Control
}

type Control struct {
	Type    byte
	Payload []byte
}

type FrameKind byte

const (
	FrameBGRA FrameKind = iota + 1
	FrameH264
)

type Frame struct {
	Kind   FrameKind
	Width  uint16
	Height uint16
	BGRA   []byte
	H264   []byte
}

type InputKind byte

const (
	InputKey InputKind = iota + 1
	InputUnicode
	InputMouse
	InputRelativeMouse
	InputExtendedMouse
	InputSynchronize
)

type Input struct {
	Kind  InputKind
	Flags uint16
	Code  uint16
	X     int16
	Y     int16
}

type Config struct {
	BindAddress    string
	Port           uint16
	Certificate    string
	PrivateKey     string
	Width          uint16
	Height         uint16
	DirectGFX      bool
	SwapAltCommand bool
}
