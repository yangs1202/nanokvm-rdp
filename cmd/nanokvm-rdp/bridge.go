package main

/*
#cgo pkg-config: --static freerdp-server3 freerdp3 winpr3
#cgo CFLAGS: -I.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "rdp_shim.h"

extern void goShimEvent(uintptr_t owner, uint32_t event, uint32_t value, uint32_t value2);
extern uint8_t goShimInput(uintptr_t owner, uint8_t type, uint8_t* payload, uint16_t length);

static NanokvmRdpCallbacks nanokvmRdpCallbacks(uintptr_t owner) {
	NanokvmRdpCallbacks callbacks = { owner, goShimEvent, goShimInput };
	return callbacks;
}
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

//export goShimEvent
func goShimEvent(owner C.uintptr_t, event C.uint32_t, value C.uint32_t, value2 C.uint32_t) {
	handle := cgo.Handle(owner)
	gateway, ok := handle.Value().(*Gateway)
	if ok {
		gateway.shimEvent(uint32(event), uint32(value), uint32(value2))
	}
}

//export goShimInput
func goShimInput(owner C.uintptr_t, inputType C.uint8_t, payload *C.uint8_t, length C.uint16_t) C.uint8_t {
	handle := cgo.Handle(owner)
	gateway, ok := handle.Value().(*Gateway)
	if !ok {
		return C.uint8_t(0)
	}
	var bytes []byte
	if payload != nil && length > 0 {
		bytes = C.GoBytes(unsafe.Pointer(payload), C.int(length))
	}
	if gateway.shimInput(uint8(inputType), bytes) {
		return C.uint8_t(1)
	}
	return C.uint8_t(0)
}

func newRDPShim(cfg Config, owner cgo.Handle) (unsafe.Pointer, error) {
	bind := C.CString(cfg.ListenAddress)
	cert := C.CString(cfg.Certificate)
	key := C.CString(cfg.PrivateKey)
	defer C.free(unsafe.Pointer(bind))
	defer C.free(unsafe.Pointer(cert))
	defer C.free(unsafe.Pointer(key))
	callbacks := C.nanokvmRdpCallbacks(C.uintptr_t(owner))
	shim := C.nanokvm_rdp_shim_new(bind, C.uint16_t(cfg.RDPPort), cert, key,
		C.uint16_t(cfg.Width), C.uint16_t(cfg.Height), C.bool(cfg.DirectGFX), &callbacks)
	if shim == nil {
		return nil, errShimCreate
	}
	return unsafe.Pointer(shim), nil
}

func runRDPShim(shim unsafe.Pointer) int {
	return int(C.nanokvm_rdp_shim_run((*C.NanokvmRdpShim)(shim)))
}

func stopRDPShim(shim unsafe.Pointer) {
	C.nanokvm_rdp_shim_stop((*C.NanokvmRdpShim)(shim))
}

func disconnectActiveRDP(shim unsafe.Pointer) {
	C.nanokvm_rdp_shim_disconnect_active((*C.NanokvmRdpShim)(shim))
}

func freeRDPShim(shim unsafe.Pointer) {
	C.nanokvm_rdp_shim_free((*C.NanokvmRdpShim)(shim))
}

func sendRDPH264(shim unsafe.Pointer, data []byte, keyframe bool) bool {
	if len(data) == 0 {
		return false
	}
	return bool(C.nanokvm_rdp_shim_send_h264((*C.NanokvmRdpShim)(shim),
		(*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data)), C.bool(keyframe)))
}

func sendRDPBGRA(shim unsafe.Pointer, data []byte) bool {
	if len(data) == 0 {
		return false
	}
	return bool(C.nanokvm_rdp_shim_send_bgra((*C.NanokvmRdpShim)(shim),
		(*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data))))
}
