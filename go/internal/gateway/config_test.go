package gateway

import "testing"

func TestParseArgsDefaultsAndOverrides(t *testing.T) {
	cfg, err := ParseArgs([]string{"-listen", "127.0.0.1:3399", "-width", "1280", "-height", "720", "-direct-gfx", "-swap-alt-command"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.BindAddress != "127.0.0.1" || cfg.RDPPort != 3399 || cfg.Width != 1280 || cfg.Height != 720 || !cfg.DirectGFX || !cfg.SwapAltCommand {
		t.Fatalf("config = %+v", cfg)
	}
	if cfg.ControlPort != 3390 || cfg.VideoPort != 5004 {
		t.Fatalf("ports = %d %d", cfg.ControlPort, cfg.VideoPort)
	}
}

func TestParseArgsRejectsMissingValue(t *testing.T) {
	if _, err := ParseArgs([]string{"-listen"}); err == nil {
		t.Fatal("missing listen value accepted")
	}
}
