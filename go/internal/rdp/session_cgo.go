package rdp

/*
#cgo pkg-config: freerdp3 freerdp-server3
#cgo CFLAGS: -Wall -Wextra
#include "bridge.h"
#include <stdlib.h>

extern void nanokvmRdpInputTrampoline(void* context, NanokvmRdpInput* input);
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime/cgo"
	"unsafe"
)

type cgoSession struct {
	bridge *C.NanokvmRdpBridge
	handle cgo.Handle
	inputs chan Input
}

func Start(config Config, inputs chan Input) (Session, error) {
	if config.BindAddress == "" || config.Port == 0 {
		return nil, errors.New("rdp listen address is incomplete")
	}
	session := &cgoSession{inputs: inputs}
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
	session.bridge = C.nanokvm_rdp_start(&cconfig, (*[0]byte)(C.nanokvmRdpInputTrampoline), unsafe.Pointer(&session.handle))
	if session.bridge == nil {
		session.handle.Delete()
		return nil, fmt.Errorf("start freerdp bridge on %s:%d", config.BindAddress, config.Port)
	}
	return session, nil
}

func (s *cgoSession) Submit(frame Frame) error {
	switch frame.Kind {
	case FrameH264:
		if len(frame.H264) == 0 {
			return errors.New("empty h264 frame")
		}
		if C.nanokvm_rdp_submit_h264(s.bridge, (*C.uint8_t)(unsafe.Pointer(&frame.H264[0])), C.size_t(len(frame.H264))) == C.bool(false) {
			return errors.New("submit h264 frame")
		}
	default:
		if len(frame.BGRA) == 0 {
			return errors.New("empty bitmap frame")
		}
		if C.nanokvm_rdp_submit_bgra(s.bridge, (*C.uint8_t)(unsafe.Pointer(&frame.BGRA[0])), C.size_t(len(frame.BGRA)), C.uint16_t(frame.Width), C.uint16_t(frame.Height)) == C.bool(false) {
			return errors.New("submit bitmap frame")
		}
	}
	return nil
}

func (s *cgoSession) Close() error {
	if s.bridge != nil {
		C.nanokvm_rdp_stop(s.bridge)
		s.bridge = nil
	}
	s.handle.Delete()
	return nil
}

//export nanokvmRdpInputTrampoline
func nanokvmRdpInputTrampoline(context unsafe.Pointer, input *C.NanokvmRdpInput) {
	handle := *(*cgo.Handle)(context)
	session := handle.Value().(*cgoSession)
	if session.inputs == nil || input == nil {
		return
	}
	select {
	case session.inputs <- Input{
		Kind:  InputKind(input.kind),
		Flags: uint16(input.flags),
		Code:  uint16(input.code),
		X:     int16(input.x),
		Y:     int16(input.y),
	}:
	default:
	}
}
