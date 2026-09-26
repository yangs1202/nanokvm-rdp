package main

import (
	"context"
	"fmt"
	"io"
	"os/exec"
	"sync"
)

type ffmpegDecoder struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	frames chan []byte
	done   chan struct{}
	stop   sync.Once
	mu     sync.Mutex
}

func startFFmpegDecoder(ctx context.Context, width, height uint16) (*ffmpegDecoder, error) {
	scale := fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black", width, height, width, height)
	cmd := exec.CommandContext(ctx, "ffmpeg", "-loglevel", "error", "-probesize", "32", "-analyzeduration", "0", "-threads", "1",
		"-flags", "low_delay", "-f", "h264", "-i", "pipe:0", "-an", "-pix_fmt", "bgra",
		"-vf", scale, "-fps_mode", "passthrough", "-flush_packets", "1", "-f", "rawvideo", "pipe:1")
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("ffmpeg stdin: %w", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("ffmpeg stdout: %w", err)
	}
	if err := cmd.Start(); err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("start ffmpeg: %w", err)
	}
	decoder := &ffmpegDecoder{
		cmd: cmd, stdin: stdin, frames: make(chan []byte, 1), done: make(chan struct{}),
	}
	frameSize := int(width) * int(height) * 4
	go func() {
		defer close(decoder.done)
		defer close(decoder.frames)
		buffer := make([]byte, frameSize)
		for {
			if _, err := io.ReadFull(stdout, buffer); err != nil {
				return
			}
			frame := append([]byte(nil), buffer...)
			select {
			case decoder.frames <- frame:
			default:
				select {
				case <-decoder.frames:
				default:
				}
				select {
				case decoder.frames <- frame:
				default:
				}
			}
		}
	}()
	return decoder, nil
}

func (d *ffmpegDecoder) push(data []byte) error {
	d.mu.Lock()
	stdin := d.stdin
	d.mu.Unlock()
	if stdin == nil {
		return io.ErrClosedPipe
	}
	for len(data) > 0 {
		n, err := stdin.Write(data)
		if err != nil {
			return fmt.Errorf("write ffmpeg input: %w", err)
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}

func (d *ffmpegDecoder) close() {
	if d == nil {
		return
	}
	d.stop.Do(func() {
		d.mu.Lock()
		stdin := d.stdin
		d.stdin = nil
		d.mu.Unlock()
		if stdin != nil {
			// Closing from outside the writer unblocks a pipe write when ffmpeg
			// stops consuming input; shutdown must not wait on the same mutex.
			_ = stdin.Close()
		}
		<-d.done
		_ = d.cmd.Wait()
	})
}
