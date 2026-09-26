package video

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestDecoderEmitsBGRAFrame(t *testing.T) {
	if !ffmpegAvailable() {
		t.Skip("ffmpeg is not available")
	}
	frames := make(chan []byte, 1)
	decoder, err := StartDecoder(16, 16, func(frame []byte) {
		select {
		case frames <- append([]byte(nil), frame...):
		default:
		}
	})
	if err != nil {
		t.Fatal(err)
	}
	defer decoder.Stop()
	path := filepath.Join(t.TempDir(), "frame.h264")
	if err := exec.Command("ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=s=16x16:r=10:d=0.3", "-c:v", "libx264", "-g", "1", "-bf", "0", "-pix_fmt", "yuv420p", "-f", "h264", path).Run(); err != nil {
		t.Fatal(err)
	}
	annexb, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := decoder.Push(annexb); err != nil {
		t.Fatal(err)
	}
	if err := decoder.CloseInput(); err != nil {
		t.Fatal(err)
	}
	select {
	case frame := <-frames:
		if len(frame) != 16*16*4 || bytes.Equal(frame, make([]byte, len(frame))) {
			t.Fatalf("frame length=%d", len(frame))
		}
	case <-time.After(2 * time.Second):
		t.Fatal("decoder produced no frame")
	}
}
