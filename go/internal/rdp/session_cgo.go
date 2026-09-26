package rdp

/*
#cgo pkg-config: freerdp3 freerdp-server3 winpr3
#cgo LDFLAGS: -ldl -lpthread ${NANOKVM_STATIC_LIBS:--lssl -lcrypto}
#cgo CFLAGS: -Wall -Wextra -Wno-deprecated-declarations
#include "session_api.h"
#include <stdlib.h>

extern bool nanokvmSendControl(void* context, unsigned char type, void* payload, unsigned short length);
extern bool nanokvmReadH264(void* context, unsigned char** data, size_t* length, unsigned int* losses);
extern bool nanokvmDecodeStart(void* context, unsigned short width, unsigned short height);
extern bool nanokvmDecodePush(void* context, unsigned char* data, size_t length);
extern void nanokvmDecodeStop(void* context);
#include <stdbool.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime/cgo"
	"unsafe"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/video"
)

type cgoSession struct {
	session  *C.NanokvmRdpSession
	handle   cgo.Handle
	inputs   chan Input
	controls chan controlEvent
	frames   chan []byte
	decoder  *video.Decoder
}

type controlEvent struct {
	kind    byte
	payload []byte
}

func Start(config Config, inputs chan Input) (Session, error) {
	if config.BindAddress == "" || config.Port == 0 {
		return nil, errors.New("rdp listen address is incomplete")
	}
	session := &cgoSession{inputs: inputs, controls: make(chan controlEvent, 32), frames: make(chan []byte, 2)}
	session.handle = cgo.NewHandle(session)
	address := C.CString(config.BindAddress)
	defer C.free(unsafe.Pointer(address))
	var cert, key *C.char
	if config.Certificate != "" {
		cert = C.CString(config.Certificate)
		defer C.free(unsafe.Pointer(cert))
	}
	if config.PrivateKey != "" {
		key = C.CString(config.PrivateKey)
		defer C.free(unsafe.Pointer(key))
	}
	cconfig := C.NanokvmRdpConfig{
		bind_address:     address,
		port:             C.uint16_t(config.Port),
		certificate:      cert,
		private_key:      key,
		width:            C.uint16_t(config.Width),
		height:           C.uint16_t(config.Height),
		direct_gfx:       C.bool(config.DirectGFX),
		swap_alt_command: C.bool(config.SwapAltCommand),
	}
	hooks := C.NanokvmSessionHooks{
		context:      unsafe.Pointer(&session.handle),
		send_control: (*[0]byte)(C.nanokvmSendControl),
		read_h264:    (*[0]byte)(C.nanokvmReadH264),
		decode_start: (*[0]byte)(C.nanokvmDecodeStart),
		decode_push:  (*[0]byte)(C.nanokvmDecodePush),
		decode_stop:  (*[0]byte)(C.nanokvmDecodeStop),
	}
	session.session = C.nanokvm_session_start(&cconfig, &hooks)
	if session.session == nil {
		session.handle.Delete()
		return nil, fmt.Errorf("start freerdp bridge on %s:%d", config.BindAddress, config.Port)
	}
	go session.pump()
	return session, nil
}

func (s *cgoSession) pump() {
	for s.session != nil && C.nanokvm_session_pump(s.session, 50) != C.bool(false) {
	}
}

func (s *cgoSession) Submit(frame Frame) error {
	if frame.Kind == FrameH264 && len(frame.H264) > 0 {
		select {
		case s.frames <- append([]byte(nil), frame.H264...):
		default:
			select {
			case <-s.frames:
			default:
			}
			s.frames <- append([]byte(nil), frame.H264...)
		}
		return nil
	}
	if len(frame.BGRA) == 0 || s.session == nil {
		return errors.New("empty bitmap frame")
	}
	ok := C.nanokvm_session_submit_bitmap(s.session, (*C.uint8_t)(unsafe.Pointer(&frame.BGRA[0])), C.size_t(len(frame.BGRA)))
	if ok == C.bool(false) {
		return errors.New("submit bitmap frame")
	}
	return nil
}

func (s *cgoSession) Close() error {
	if s.session != nil {
		C.nanokvm_session_stop(s.session)
		s.session = nil
	}
	s.handle.Delete()
	return nil
}

func (s *cgoSession) Controls() <-chan Control {
	out := make(chan Control)
	go func() {
		defer close(out)
		for event := range s.controls {
			out <- Control{Type: event.kind, Payload: event.payload}
		}
	}()
	return out
}

//export nanokvmSendControl
func nanokvmSendControl(context unsafe.Pointer, kind C.uint8_t, payload unsafe.Pointer, length C.uint16_t) C.bool {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	event := controlEvent{kind: byte(kind)}
	if length > 0 && payload != nil {
		event.payload = C.GoBytes(payload, C.int(length))
	}
	select {
	case session.controls <- event:
		return C.bool(true)
	default:
		return C.bool(false)
	}
}

//export nanokvmReadH264
func nanokvmReadH264(context unsafe.Pointer, data **C.uint8_t, length *C.size_t, losses *C.uint32_t) C.bool {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	select {
	case frame := <-session.frames:
		if len(frame) == 0 {
			return C.bool(false)
		}
		buf := C.malloc(C.size_t(len(frame)))
		copy(unsafe.Slice((*byte)(buf), len(frame)), frame)
		*data = (*C.uint8_t)(buf)
		*length = C.size_t(len(frame))
		return C.bool(true)
	default:
		return C.bool(false)
	}
}

//export nanokvmDecodeStart
func nanokvmDecodeStart(context unsafe.Pointer, width C.uint16_t, height C.uint16_t) C.bool {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	decoder, err := video.StartDecoder(uint16(width), uint16(height), func(frame []byte) {
		if session.session == nil {
			return
		}
		C.nanokvm_session_submit_bitmap(session.session, (*C.uint8_t)(unsafe.Pointer(&frame[0])), C.size_t(len(frame)))
	})
	if err != nil {
		return C.bool(false)
	}
	session.decoder = decoder
	return C.bool(true)
}

//export nanokvmDecodePush
func nanokvmDecodePush(context unsafe.Pointer, data *C.uint8_t, length C.size_t) C.bool {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	if session.decoder == nil || length == 0 {
		return C.bool(false)
	}
	if err := session.decoder.Push(C.GoBytes(unsafe.Pointer(data), C.int(length))); err != nil {
		return C.bool(false)
	}
	return C.bool(true)
}

//export nanokvmDecodeStop
func nanokvmDecodeStop(context unsafe.Pointer) {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	if session.decoder != nil {
		session.decoder.Stop()
		session.decoder = nil
	}
}
