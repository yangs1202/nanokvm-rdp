package video

import (
	"fmt"
	"io"
	"os/exec"
	"sync"
)

type Decoder struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	cancel chan struct{}
	done   sync.WaitGroup
}

func ffmpegAvailable() bool {
	_, err := exec.LookPath("ffmpeg")
	return err == nil
}

func StartDecoder(width, height uint16, emit func([]byte)) (*Decoder, error) {
	if width == 0 || height == 0 || emit == nil {
		return nil, fmt.Errorf("decoder size is invalid")
	}
	scale := fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black", width, height, width, height)
	cmd := exec.Command("ffmpeg", "-loglevel", "error", "-fflags", "nobuffer", "-avioflags", "direct", "-probesize", "32", "-analyzeduration", "0", "-threads", "1", "-flags", "low_delay", "-f", "h264", "-i", "pipe:0", "-an", "-pix_fmt", "bgra", "-vf", scale, "-enc_time_base", "1:30", "-fps_mode", "passthrough", "-flush_packets", "1", "-f", "rawvideo", "pipe:1")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	decoder := &Decoder{cmd: cmd, stdin: stdin, cancel: make(chan struct{})}
	decoder.done.Add(1)
	go func() {
		defer decoder.done.Done()
		frame := make([]byte, int(width)*int(height)*4)
		for {
			if _, err := io.ReadFull(stdout, frame); err != nil {
				return
			}
			emit(frame)
		}
	}()
	return decoder, nil
}

func (d *Decoder) Push(annexb []byte) error {
	_, err := d.stdin.Write(annexb)
	return err
}

func (d *Decoder) CloseInput() error { return d.stdin.Close() }

func (d *Decoder) Stop() {
	_ = d.stdin.Close()
	d.done.Wait()
	_ = d.cmd.Wait()
}
