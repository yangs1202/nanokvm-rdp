package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/yangs1202/nanokvm-rdp/go/go/internal/gateway"
)

func main() {
	cfg, err := gateway.ParseArgs(os.Args[1:])
	if err != nil {
		slog.Error("invalid arguments", "err", err)
		os.Exit(2)
	}
	slog.Info("nanokvm rdp gateway", "listen", cfg.BindAddress, "port", cfg.RDPPort, "control", cfg.ControlPort, "video", cfg.VideoPort)
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := gateway.Run(ctx, cfg); err != nil {
		slog.Error("gateway stopped", "err", err)
		os.Exit(1)
	}
}
