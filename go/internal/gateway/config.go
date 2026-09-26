package gateway

import (
	"fmt"
	"strconv"
	"strings"
)

type Config struct {
	BindAddress    string
	RDPPort        uint16
	Certificate    string
	PrivateKey     string
	Width          uint16
	Height         uint16
	Bitrate        uint16
	ControlPort    uint16
	VideoPort      uint16
	DirectGFX      bool
	SwapAltCommand bool
}

func ParseArgs(args []string) (Config, error) {
	cfg := Config{
		BindAddress: "0.0.0.0",
		RDPPort:     3389,
		Certificate: "/root/nanokvm-rdp/cert.pem",
		PrivateKey:  "/root/nanokvm-rdp/key.pem",
		Width:       1920,
		Height:      1080,
		Bitrate:     8000,
		ControlPort: 3390,
		VideoPort:   5004,
	}
	for i := 0; i < len(args); i++ {
		need := func() (string, error) {
			if i+1 >= len(args) {
				return "", fmt.Errorf("missing value for %s", args[i])
			}
			i++
			return args[i], nil
		}
		switch args[i] {
		case "-listen":
			value, err := need()
			if err != nil {
				return Config{}, err
			}
			host, port, err := splitHostPort(value)
			if err != nil {
				return Config{}, err
			}
			cfg.BindAddress, cfg.RDPPort = host, port
		case "-cert":
			value, err := need()
			if err != nil {
				return Config{}, err
			}
			cfg.Certificate = value
		case "-key":
			value, err := need()
			if err != nil {
				return Config{}, err
			}
			cfg.PrivateKey = value
		case "-width":
			value, err := parsePort(need)
			if err != nil {
				return Config{}, err
			}
			cfg.Width = value
		case "-height":
			value, err := parsePort(need)
			if err != nil {
				return Config{}, err
			}
			cfg.Height = value
		case "-bitrate":
			value, err := parsePort(need)
			if err != nil {
				return Config{}, err
			}
			cfg.Bitrate = value
		case "-control-port":
			value, err := parsePort(need)
			if err != nil {
				return Config{}, err
			}
			cfg.ControlPort = value
		case "-video-port":
			value, err := parsePort(need)
			if err != nil {
				return Config{}, err
			}
			cfg.VideoPort = value
		case "-direct-gfx":
			cfg.DirectGFX = true
		case "-swap-alt-command":
			cfg.SwapAltCommand = true
		default:
			return Config{}, fmt.Errorf("unknown argument %s", args[i])
		}
	}
	if cfg.Width == 0 || cfg.Height == 0 || cfg.ControlPort == 0 || cfg.VideoPort == 0 {
		return Config{}, fmt.Errorf("width, height, and ports must be non-zero")
	}
	return cfg, nil
}

func splitHostPort(value string) (string, uint16, error) {
	host, portText, ok := strings.Cut(value, ":")
	if !ok || host == "" || strings.Contains(portText, ":") {
		return "", 0, fmt.Errorf("listen must be host:port")
	}
	port, err := strconv.ParseUint(portText, 10, 16)
	if err != nil || port == 0 {
		return "", 0, fmt.Errorf("invalid listen port")
	}
	return host, uint16(port), nil
}

func parsePort(need func() (string, error)) (uint16, error) {
	value, err := need()
	if err != nil {
		return 0, err
	}
	parsed, err := strconv.ParseUint(value, 10, 16)
	if err != nil || parsed == 0 {
		return 0, fmt.Errorf("invalid number %s", value)
	}
	return uint16(parsed), nil
}
