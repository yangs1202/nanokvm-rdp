package main

import (
	"flag"
	"fmt"
	"log/slog"
	"net"
	"os"
	"strconv"
	"strings"
)

func main() {
	listen := flag.String("listen", "0.0.0.0:3389", "RDP listen address")
	cert := flag.String("cert", "/root/nanokvm-rdp/cert.pem", "RDP certificate")
	key := flag.String("key", "/root/nanokvm-rdp/key.pem", "RDP private key")
	width := flag.Uint("width", 1920, "source width")
	height := flag.Uint("height", 1080, "source height")
	_ = flag.Uint("bitrate", 8000, "compatibility option; bitrate is controlled by the agent")
	controlPort := flag.Uint("control-port", 3390, "agent control port")
	videoPort := flag.Uint("video-port", 5004, "agent RTP port")
	directGFX := flag.Bool("direct-gfx", false, "use RDPGFX AVC420/Progressive")
	swapAltCommand := flag.Bool("swap-alt-command", false, "swap Alt and Command scan codes")
	flag.Parse()

	if *width == 0 || *height == 0 || *width > 65535 || *height > 65535 || *controlPort == 0 || *controlPort > 65535 || *videoPort == 0 || *videoPort > 65535 {
		flag.Usage()
		os.Exit(2)
	}
	host, port, err := splitListen(*listen)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))
	gateway, err := NewGateway(Config{
		ListenAddress: host, RDPPort: uint16(port), ControlPort: uint16(*controlPort), VideoPort: uint16(*videoPort),
		Certificate: *cert, PrivateKey: *key, Width: uint16(*width), Height: uint16(*height),
		DirectGFX: *directGFX, SwapAltCommand: *swapAltCommand,
	}, logger)
	if err != nil {
		logger.Error("create gateway", "error", err)
		os.Exit(1)
	}
	if err := runUntilSignal(gateway); err != nil {
		logger.Error("gateway stopped", "error", err)
		os.Exit(1)
	}
}

func splitListen(value string) (string, int, error) {
	separator := strings.LastIndexByte(value, ':')
	if separator <= 0 || separator == len(value)-1 {
		return "", 0, fmt.Errorf("listen must be host:port: %q", value)
	}
	port, err := strconv.Atoi(value[separator+1:])
	if err != nil || port < 1 || port > 65535 {
		return "", 0, fmt.Errorf("invalid listen port: %q", value[separator+1:])
	}
	if net.ParseIP(value[:separator]) == nil {
		return "", 0, fmt.Errorf("listen host must be an IPv4 or IPv6 address: %q", value[:separator])
	}
	return value[:separator], port, nil
}
