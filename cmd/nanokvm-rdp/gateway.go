package main

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"runtime/cgo"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unicode/utf8"
	"unsafe"
)

var errShimCreate = errors.New("create FreeRDP shim")

type Config struct {
	ListenAddress  string
	RDPPort        uint16
	ControlPort    uint16
	VideoPort      uint16
	Certificate    string
	PrivateKey     string
	Width          uint16
	Height         uint16
	DirectGFX      bool
	SwapAltCommand bool
}

type agentConnection struct {
	conn         net.Conn
	writeMu      sync.Mutex
	lastActivity atomic.Int64
}

type frameFlow struct {
	mu      sync.Mutex
	pending map[uint32]time.Time
}

func (f *frameFlow) ack(id uint32) {
	f.mu.Lock()
	delete(f.pending, id)
	f.mu.Unlock()
}

type Gateway struct {
	cfg Config
	log *slog.Logger

	owner cgo.Handle
	shim  unsafe.Pointer

	controlListener net.Listener
	controlMu       sync.Mutex
	agent           *agentConnection
	controlConns    map[net.Conn]struct{}
	workers         sync.WaitGroup
	closed          atomic.Bool
	streamRequested atomic.Bool

	mediaMu       sync.Mutex
	mediaCancel   context.CancelFunc
	mediaCtx      context.Context
	mediaConn     *net.UDPConn
	mediaWG       sync.WaitGroup
	frameWG       sync.WaitGroup
	decoder       *ffmpegDecoder
	mode          atomic.Uint32
	session       atomic.Bool
	needDirectIDR atomic.Bool

	inputMu        sync.Mutex
	keyboardMods   byte
	pointerButtons byte
	controlSpace   bool
	frameFlow      frameFlow
	mediaUnits     atomic.Uint64
	decodedFrames  atomic.Uint64

	stopOnce   sync.Once
	stop       chan struct{}
	done       chan struct{}
	shimDone   chan struct{}
	shimStatus chan error
	started    atomic.Bool
}

const (
	mediaUnknown uint32 = iota
	mediaAVC420
	mediaBitmap
)

const (
	shimSessionStarted uint32 = 1
	shimSessionStopped uint32 = 2
	shimVideoAVC420    uint32 = 3
	shimVideoBitmap    uint32 = 4
	shimFrameAck       uint32 = 5
	shimH264Overflow   uint32 = 7
)

func NewGateway(cfg Config, logger *slog.Logger) (*Gateway, error) {
	if net.ParseIP(cfg.ListenAddress) == nil {
		return nil, fmt.Errorf("listen host must be an IPv4 or IPv6 address: %q", cfg.ListenAddress)
	}
	if logger == nil {
		logger = slog.Default()
	}
	gateway := &Gateway{cfg: cfg, log: logger, controlConns: make(map[net.Conn]struct{}), stop: make(chan struct{}), done: make(chan struct{}), shimDone: make(chan struct{}), shimStatus: make(chan error, 1)}
	gateway.frameFlow.pending = make(map[uint32]time.Time)
	gateway.owner = cgo.NewHandle(gateway)
	shim, err := newRDPShim(cfg, gateway.owner)
	if err != nil {
		gateway.owner.Delete()
		return nil, err
	}
	gateway.shim = shim
	return gateway, nil
}

func (g *Gateway) Run(ctx context.Context) error {
	if err := g.listenControl(); err != nil {
		g.Close()
		return err
	}
	if !g.started.CompareAndSwap(false, true) {
		return errors.New("gateway already started")
	}
	go g.runShim()
	g.workers.Add(2)
	go g.controlAcceptLoop()
	go g.heartbeatLoop()
	select {
	case <-ctx.Done():
		g.Close()
	case <-g.stop:
		g.Close()
	case err := <-g.shimStatus:
		g.Close()
		return err
	}
	<-g.done
	return nil
}

func (g *Gateway) runShim() {
	defer close(g.shimDone)
	if status := runRDPShim(g.shim); status != 0 {
		g.log.Error("FreeRDP shim stopped", "status", status)
		g.shimStatus <- fmt.Errorf("FreeRDP shim exited with status %d", status)
		return
	}
	g.shimStatus <- nil
}

func (g *Gateway) Close() {
	g.stopOnce.Do(func() {
		close(g.stop)
		g.controlMu.Lock()
		g.closed.Store(true)
		listener := g.controlListener
		connections := make([]net.Conn, 0, len(g.controlConns))
		for conn := range g.controlConns {
			connections = append(connections, conn)
		}
		g.controlMu.Unlock()
		if listener != nil {
			_ = listener.Close()
		}
		for _, conn := range connections {
			_ = conn.Close()
		}
		g.workers.Wait()
		g.closeAgent()
		g.session.Store(false)
		g.stopMedia()
		if g.started.Load() {
			stopRDPShim(g.shim)
			<-g.shimDone
		}
		freeRDPShim(g.shim)
		g.owner.Delete()
		close(g.done)
	})
}

func (g *Gateway) listenControl() error {
	listener, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.ParseIP(g.cfg.ListenAddress), Port: int(g.cfg.ControlPort)})
	if err != nil {
		return fmt.Errorf("listen control: %w", err)
	}
	g.controlListener = listener
	return nil
}

func (g *Gateway) controlAcceptLoop() {
	defer g.workers.Done()
	for {
		conn, err := g.controlListener.Accept()
		if err != nil {
			select {
			case <-g.stop:
				return
			default:
			}
			g.log.Warn("agent accept failed", "error", err)
			continue
		}
		if tcp, ok := conn.(*net.TCPConn); ok {
			_ = tcp.SetNoDelay(true)
		}
		g.controlMu.Lock()
		if g.closed.Load() {
			g.controlMu.Unlock()
			_ = conn.Close()
			return
		}
		g.controlConns[conn] = struct{}{}
		g.workers.Add(1)
		g.controlMu.Unlock()
		go g.handleAgentTracked(conn)
	}
}

func (g *Gateway) handleAgent(conn net.Conn) {
	g.handleAgentLoop(conn, false)
}

func (g *Gateway) handleAgentTracked(conn net.Conn) {
	g.handleAgentLoop(conn, true)
}

func (g *Gateway) handleAgentLoop(conn net.Conn, tracked bool) {
	if tracked {
		defer g.workers.Done()
	}
	defer func() {
		g.controlMu.Lock()
		delete(g.controlConns, conn)
		g.controlMu.Unlock()
	}()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	message, err := readControl(conn)
	if err != nil || message.typ != controlHello || (len(message.payload) != helloBaseLength && len(message.payload) != helloCapsLength) {
		_ = conn.Close()
		return
	}
	_ = conn.SetReadDeadline(time.Time{})
	agent := &agentConnection{conn: conn}
	agent.lastActivity.Store(time.Now().UnixMilli())
	g.controlMu.Lock()
	if g.closed.Load() {
		g.controlMu.Unlock()
		_ = conn.Close()
		return
	}
	previous := g.agent
	g.agent = agent
	start := g.streamRequested.Load()
	g.controlMu.Unlock()
	if previous != nil {
		_ = previous.conn.Close()
	}
	g.log.Info("agent control connected", "key_ack", len(message.payload) == helloCapsLength && message.payload[8]&keyAckCapability != 0)
	if start {
		_ = g.sendAgent(controlStartStream, nil)
	}
	defer func() {
		_ = conn.Close()
		g.controlMu.Lock()
		if g.agent == agent {
			g.agent = nil
		}
		g.controlMu.Unlock()
	}()
	for {
		message, err := readControl(conn)
		if err != nil {
			return
		}
		agent.lastActivity.Store(time.Now().UnixMilli())
		switch message.typ {
		case controlPing:
			_ = sendAgentTo(agent, controlPong, nil)
		case controlStats:
			if len(message.payload) == 16 {
				g.log.Debug("agent stats", "packets", binary.BigEndian.Uint32(message.payload), "dropped", binary.BigEndian.Uint32(message.payload[4:]))
			}
		case controlError:
			g.log.Warn("agent reported error", "payload_length", len(message.payload))
		case controlKeyAck:
			g.log.Debug("agent key ack", "ok", len(message.payload) == 5 && message.payload[4] != 0)
		}
	}
}

func (g *Gateway) heartbeatLoop() {
	defer g.workers.Done()
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			g.controlMu.Lock()
			agent := g.agent
			g.controlMu.Unlock()
			if agent == nil {
				continue
			}
			age := time.Now().UnixMilli() - agent.lastActivity.Load()
			if age > 5000 {
				_ = agent.conn.Close()
				g.log.Warn("agent heartbeat timeout", "age_ms", age)
				g.requestStream(false)
				disconnectActiveRDP(g.shim)
				continue
			}
			_ = g.sendAgent(controlPing, nil)
		case <-g.stop:
			return
		}
	}
}

func (g *Gateway) sendAgent(typ byte, payload []byte) error {
	g.controlMu.Lock()
	agent := g.agent
	g.controlMu.Unlock()
	if agent == nil {
		return errors.New("agent control is disconnected")
	}
	return sendAgentTo(agent, typ, payload)
}

func sendAgentTo(agent *agentConnection, typ byte, payload []byte) error {
	agent.writeMu.Lock()
	defer agent.writeMu.Unlock()
	_ = agent.conn.SetWriteDeadline(time.Now().Add(100 * time.Millisecond))
	if err := writeControl(agent.conn, typ, payload); err != nil {
		_ = agent.conn.Close()
		return err
	}
	return nil
}

func (g *Gateway) closeAgent() {
	g.controlMu.Lock()
	agent := g.agent
	g.agent = nil
	g.controlMu.Unlock()
	if agent != nil {
		_ = agent.conn.Close()
	}
}

func (g *Gateway) requestStream(start bool) {
	previous := g.streamRequested.Swap(start)
	if previous == start {
		return
	}
	typ := controlStopStream
	if start {
		typ = controlStartStream
	}
	if err := g.sendAgent(typ, nil); err != nil {
		g.log.Warn("agent stream request failed", "start", start, "error", err)
	}
}

func (g *Gateway) shimEvent(event, value, value2 uint32) {
	if g.closed.Load() {
		return
	}
	switch event {
	case shimSessionStarted:
		g.session.Store(true)
		g.needDirectIDR.Store(true)
		g.mode.Store(map[bool]uint32{true: mediaAVC420, false: mediaBitmap}[g.cfg.DirectGFX])
		if g.startMedia() {
			if g.mode.Load() == mediaBitmap {
				g.startDecoder()
			}
			g.requestStream(true)
		} else {
			g.session.Store(false)
			disconnectActiveRDP(g.shim)
		}
	case shimSessionStopped:
		if g.session.Swap(false) {
			g.needDirectIDR.Store(false)
			g.requestStream(false)
			g.stopMedia()
			g.releaseAllInputs()
		}
	case shimVideoAVC420:
		g.log.Info("RDPGFX video mode", "codec", "AVC420")
		g.needDirectIDR.Store(true)
		g.mode.Store(mediaAVC420)
		g.stopDecoder()
	case shimVideoBitmap:
		g.log.Info("RDP video mode", "codec", "bitmap/progressive")
		g.mode.Store(mediaBitmap)
		g.startDecoder()
	case shimFrameAck:
		g.frameFlow.ack(value)
	case shimH264Overflow:
		g.log.Warn("H264 shim queue overflow; requesting IDR", "capacity", value, "aux", value2)
		g.needDirectIDR.Store(true)
		_ = g.sendAgent(controlIDRRequest, nil)
	}
}

func (g *Gateway) startMedia() bool {
	g.mediaMu.Lock()
	if g.mediaCancel != nil {
		g.mediaMu.Unlock()
		return true
	}
	ctx, cancel := context.WithCancel(context.Background())
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP(g.cfg.ListenAddress), Port: int(g.cfg.VideoPort)})
	if err != nil {
		cancel()
		g.mediaMu.Unlock()
		g.log.Error("listen RTP", "error", err)
		return false
	}
	g.mediaCancel = cancel
	g.mediaCtx = ctx
	g.mediaConn = conn
	g.mediaWG.Add(1)
	g.mediaMu.Unlock()
	go g.mediaLoop(ctx, conn)
	return true
}

func (g *Gateway) stopMedia() {
	g.mediaMu.Lock()
	cancel := g.mediaCancel
	conn := g.mediaConn
	g.mediaCancel = nil
	decoder := g.decoder
	g.decoder = nil
	g.mediaCtx = nil
	g.mediaConn = nil
	g.mediaMu.Unlock()
	if cancel != nil {
		cancel()
	}
	if conn != nil {
		_ = conn.Close()
	}
	if decoder != nil {
		decoder.close()
	}
	if cancel != nil {
		g.mediaWG.Wait()
	}
	g.frameWG.Wait()
}

func (g *Gateway) startDecoder() {
	g.mediaMu.Lock()
	if g.decoder != nil || !g.session.Load() {
		g.mediaMu.Unlock()
		return
	}
	decoder, err := startFFmpegDecoder(g.mediaCtx, g.cfg.Width, g.cfg.Height)
	if err == nil {
		g.decoder = decoder
		g.frameWG.Add(1)
	}
	g.mediaMu.Unlock()
	if err != nil {
		g.log.Error("start ffmpeg decoder failed", "error", err)
	}
	if decoder != nil {
		go func() {
			defer g.frameWG.Done()
			for frame := range decoder.frames {
				if g.decodedFrames.Add(1) == 1 {
					g.log.Info("FFmpeg decoded first BGRA frame", "bytes", len(frame))
				}
				if !g.session.Load() || g.mode.Load() != mediaBitmap {
					continue
				}
				if !sendRDPBGRA(g.shim, frame) {
					g.log.Warn("bitmap frame enqueue failed")
				}
			}
		}()
	}
}

func (g *Gateway) stopDecoder() {
	g.mediaMu.Lock()
	decoder := g.decoder
	g.decoder = nil
	g.mediaMu.Unlock()
	if decoder != nil {
		decoder.close()
		g.frameWG.Wait()
	}
}

func (g *Gateway) mediaLoop(ctx context.Context, conn *net.UDPConn) {
	defer g.mediaWG.Done()
	defer conn.Close()
	assembler := &rtpH264Assembler{}
	direct := directH264State{needIDR: g.needDirectIDR.Load()}
	packet := make([]byte, 2048)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, _, err := conn.ReadFromUDP(packet)
		if err != nil {
			if errors.Is(err, net.ErrClosed) || errors.Is(ctx.Err(), context.Canceled) {
				return
			}
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				select {
				case <-ctx.Done():
					return
				default:
					continue
				}
			}
			g.log.Warn("read RTP", "error", err)
			continue
		}
		select {
		case <-ctx.Done():
			return
		default:
		}
		unit, ready, err := assembler.push(packet[:n])
		if err != nil {
			g.log.Warn("drop invalid RTP", "error", err)
			continue
		}
		if assembler.lastLoss {
			direct.needIDR = true
			g.needDirectIDR.Store(true)
			_ = g.sendAgent(controlIDRRequest, nil)
		}
		if !ready || len(unit.data) == 0 {
			continue
		}
		if g.mediaUnits.Add(1) == 1 {
			g.log.Info("received first RTP H264 access unit", "bytes", len(unit.data), "keyframe", unit.keyframe)
		}
		if g.mode.Load() == mediaAVC420 {
			if g.needDirectIDR.Load() {
				direct.needIDR = true
			}
			payload, send := direct.prepare(unit)
			if !send {
				continue
			}
			if !direct.needIDR {
				g.needDirectIDR.Store(false)
			}
			if !sendRDPH264(g.shim, payload, unit.keyframe) {
				g.log.Warn("H264 frame enqueue failed")
				g.needDirectIDR.Store(true)
			}
		} else if g.mode.Load() == mediaBitmap {
			g.mediaMu.Lock()
			decoder := g.decoder
			g.mediaMu.Unlock()
			if decoder == nil {
				g.startDecoder()
				continue
			}
			if err := decoder.push(unit.data); err != nil {
				g.log.Error("FFmpeg decoder input failed", "error", err)
				g.stopDecoder()
				_ = g.sendAgent(controlIDRRequest, nil)
			}
		}
	}
}

func (g *Gateway) shimInput(inputType uint8, payload []byte) bool {
	switch inputType {
	case controlKey:
		return g.forwardKey(payload)
	case controlTextUTF8:
		return g.forwardUnicode(payload)
	case controlPointerAbs:
		return g.forwardAbsolute(payload)
	case controlPointerRel:
		return g.forwardRelative(payload)
	case controlSynchronize:
		g.inputMu.Lock()
		g.keyboardMods, g.pointerButtons, g.controlSpace = 0, 0, false
		g.inputMu.Unlock()
		return g.sendAgent(controlSynchronize, nil) == nil
	default:
		return false
	}
}

func (g *Gateway) forwardKey(payload []byte) bool {
	if len(payload) != 3 {
		return false
	}
	code, extended, release := payload[0], payload[1] != 0, payload[2] != 0
	if g.cfg.SwapAltCommand {
		switch {
		case code == 0x38:
			code, extended = map[bool]struct {
				code     byte
				extended bool
			}{false: {0x5b, true}, true: {0x5c, true}}[extended].code, true
		case extended && code == 0x5b:
			code, extended = 0x38, false
		case extended && code == 0x5c:
			code, extended = 0x38, true
		}
	}
	g.inputMu.Lock()
	modifier := modifierForScancode(code, extended)
	if release {
		g.keyboardMods &^= modifier
	} else {
		g.keyboardMods |= modifier
	}
	if code == 0x39 && !extended {
		g.controlSpace = !release
	}
	g.inputMu.Unlock()
	return g.sendAgent(controlKey, []byte{code, boolByte(extended), boolByte(release)}) == nil
}

func (g *Gateway) forwardUnicode(payload []byte) bool {
	if len(payload) != 2 {
		return false
	}
	code := rune(binary.BigEndian.Uint16(payload))
	if code == 0 || (code >= 0xd800 && code <= 0xdfff) {
		return false
	}
	if !utf8.ValidRune(code) || utf8.RuneLen(code) <= 0 {
		return false
	}
	encoded := make([]byte, utf8.RuneLen(code))
	n := utf8.EncodeRune(encoded, code)
	return g.sendAgent(controlTextUTF8, encoded[:n]) == nil
}

func (g *Gateway) forwardAbsolute(payload []byte) bool {
	if len(payload) != 10 {
		return false
	}
	x, y := u16(payload), u16(payload[2:])
	width, height, flags := u16(payload[4:]), u16(payload[6:]), u16(payload[8:])
	if flags&(0x0200|0x0400) == 0 {
		g.inputMu.Lock()
		g.pointerButtons = pointerButtons(g.pointerButtons, flags)
		g.inputMu.Unlock()
		out := make([]byte, 12)
		putU16(out, clampAbsolute(x, width))
		putU16(out[2:], clampAbsolute(y, height))
		putU16(out[4:], width)
		putU16(out[6:], height)
		putU16(out[8:], flags)
		if err := g.sendAgent(controlPointerAbs, out); err != nil {
			return false
		}
	}
	if flags&(0x0200|0x0400) != 0 {
		return g.sendAgent(controlWheel, payload[8:10]) == nil
	}
	return true
}

func (g *Gateway) forwardRelative(payload []byte) bool {
	if len(payload) != 6 {
		return false
	}
	dx, dy, flags := int16(u16(payload)), int16(u16(payload[2:])), u16(payload[4:])
	verticalWheel := flags&0x0200 != 0
	horizontalWheel := flags&0x0400 != 0
	if !horizontalWheel && verticalWheel && dx != 0 && dy == 0 {
		flags = (flags &^ (0x0200 | 0x01ff)) | 0x0400 | (flags & 0x01ff)
		if dx < 0 {
			flags |= 0x0100
		} else {
			flags &^= 0x0100
		}
	}
	if flags&(0x0200|0x0400) != 0 {
		wheel := make([]byte, 2)
		putU16(wheel, flags)
		return g.sendAgent(controlWheel, wheel) == nil
	}
	g.inputMu.Lock()
	g.pointerButtons = pointerButtons(g.pointerButtons, flags)
	buttons := g.pointerButtons
	g.inputMu.Unlock()
	out := []byte{payload[0], payload[1], payload[2], payload[3], buttons}
	_ = dx
	_ = dy
	return g.sendAgent(controlPointerRel, out) == nil
}

func (g *Gateway) releaseAllInputs() {
	g.inputMu.Lock()
	g.keyboardMods, g.pointerButtons, g.controlSpace = 0, 0, false
	g.inputMu.Unlock()
	_ = g.sendAgent(controlReleaseAll, nil)
}

func boolByte(value bool) byte {
	if value {
		return 1
	}
	return 0
}

func modifierForScancode(code byte, extended bool) byte {
	if extended && code == 0x1d {
		return 0x10
	}
	if extended && code == 0x38 {
		return 0x40
	}
	switch code {
	case 0x1d:
		return 0x01
	case 0x2a:
		return 0x02
	case 0x36:
		return 0x20
	case 0x38:
		return 0x04
	}
	return 0
}

func pointerButtons(buttons byte, flags uint16) byte {
	bits := []uint16{0x1000, 0x2000, 0x4000, 0x0001, 0x0002}
	for i, bit := range bits {
		if flags&bit == 0 {
			continue
		}
		if flags&0x8000 != 0 {
			buttons |= 1 << i
		} else {
			buttons &^= 1 << i
		}
	}
	return buttons
}

func clampAbsolute(value, dimension uint16) uint16 {
	if dimension == 0 {
		return 0
	}
	if value >= dimension {
		return dimension - 1
	}
	return value
}

func runUntilSignal(g *Gateway) error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return g.Run(ctx)
}
