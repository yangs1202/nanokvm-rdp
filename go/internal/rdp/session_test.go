package rdp

import (
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestBridgeBindsWithoutCertificate(t *testing.T) {
	session, err := Start(Config{BindAddress: "127.0.0.1", Port: 0, Width: 64, Height: 64}, nil)
	if err == nil {
		_ = session.Close()
		t.Fatal("port 0 should not start a listener")
	}
}

func TestBridgeStartsWithCertificate(t *testing.T) {
	dir := t.TempDir()
	cert := filepath.Join(dir, "tls.crt")
	key := filepath.Join(dir, "tls.key")
	cmd := exec.Command("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
		"-subj", "/CN=nanokvm-rdp-gateway-test", "-keyout", key, "-out", cert)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("create test certificate: %v\n%s", err, output)
	}
	if _, err := os.Stat(cert); err != nil {
		t.Fatal(err)
	}

	session, err := Start(Config{
		BindAddress:  "127.0.0.1",
		Port:         freePort(t),
		Certificate:  cert,
		PrivateKey:   key,
		Width:        64,
		Height:       64,
		DirectGFX:    true,
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := session.Close(); err != nil {
		t.Fatal(err)
	}
}

func freePort(t *testing.T) uint16 {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	return uint16(listener.Addr().(*net.TCPAddr).Port)
}

func TestFakeSessionSubmitsBitmapAndInput(t *testing.T) {
	var frames int
	var keys int
	s := &fakeSession{
		submit:  func(Frame) error { frames++; return nil },
		onInput: func(Input) { keys++ },
	}
	if err := s.Submit(Frame{Width: 2, Height: 1, BGRA: []byte{1, 2, 3, 4, 5, 6, 7, 8}}); err != nil {
		t.Fatal(err)
	}
	s.emit(Input{Kind: InputKey, Code: 0x1e})
	if frames != 1 || keys != 1 {
		t.Fatalf("frames=%d keys=%d", frames, keys)
	}
}

type fakeSession struct {
	submit  func(Frame) error
	onInput func(Input)
}

func (f *fakeSession) Submit(frame Frame) error { return f.submit(frame) }
func (f *fakeSession) Controls() <-chan Control { return nil }
func (f *fakeSession) emit(input Input) {
	if f.onInput != nil {
		f.onInput(input)
	}
}
