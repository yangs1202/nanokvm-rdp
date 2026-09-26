package main

import (
	"log/slog"
	"os"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/gateway"
)

func main() {
	cfg, err := gateway.ParseArgs(os.Args[1:])
	if err != nil {
		slog.Error("invalid arguments", "err", err)
		os.Exit(2)
	}
	slog.Info("nanokvm rdp gateway", "listen", cfg.BindAddress, "port", cfg.RDPPort, "control", cfg.ControlPort, "video", cfg.VideoPort)
}
