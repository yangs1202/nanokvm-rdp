#!/usr/bin/env python3
"""Exercise gateway shutdown with an active idle and TLS-negotiating RDP peer."""

from __future__ import annotations

import argparse
import socket
import subprocess
import time
from pathlib import Path


RDP_NEGOTIATION_REQUEST = bytes.fromhex("030000130ee000000000000100080001000000")


def wait_for_listener(process: subprocess.Popen[bytes], port: int, deadline: float) -> socket.socket:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output, _ = process.communicate()
            raise RuntimeError(
                f"gateway exited {process.returncode}: "
                f"{output.decode(errors='replace')[-1000:]}"
            )
        try:
            peer = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            peer.settimeout(1.0)
            return peer
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"gateway did not listen on {port}")


def read_exact(peer: socket.socket, length: int, deadline: float) -> bytes:
    result = bytearray()
    while len(result) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"short RDP response: got {len(result)} of {length} bytes")
        peer.settimeout(remaining)
        chunk = peer.recv(length - len(result))
        if not chunk:
            raise EOFError(f"peer closed during RDP response: got {len(result)} of {length} bytes")
        result.extend(chunk)
    return bytes(result)


def run_case(args: argparse.Namespace, label: str, negotiate: bool) -> None:
    command = [
        str(args.gateway),
        "-listen",
        f"127.0.0.1:{args.rdp_port}",
        "-cert",
        str(args.cert),
        "-key",
        str(args.key),
        "-control-port",
        str(args.control_port),
        "-video-port",
        str(args.video_port),
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    peer = None
    try:
        peer = wait_for_listener(process, args.rdp_port, time.monotonic() + args.timeout)
        if negotiate:
            peer.sendall(RDP_NEGOTIATION_REQUEST)
            response = read_exact(peer, 19, time.monotonic() + args.timeout)
            if (
                response[:4] != b"\x03\x00\x00\x13"
                or response[5] != 0xD0
                or response[11] != 2
                or response[13:15] != b"\x08\x00"
                or response[15:19] != b"\x01\x00\x00\x00"
            ):
                raise RuntimeError(f"unexpected RDP negotiation response: {response.hex()}")
            print(f"{label}: negotiation response={response.hex()}")
        shutdown_started = time.monotonic()
        process.terminate()
        output, _ = process.communicate(timeout=args.timeout)
        elapsed_ms = (time.monotonic() - shutdown_started) * 1000.0
        if process.returncode != 0:
            raise RuntimeError(
                f"{label}: gateway exited {process.returncode}: "
                f"{output.decode(errors='replace')[-1000:]}"
            )
        print(f"{label}: shutdown {elapsed_ms:.1f} ms, exit={process.returncode}")
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.communicate()
        raise
    finally:
        if peer is not None:
            peer.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", type=Path, required=True)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--rdp-port", type=int, required=True)
    parser.add_argument("--control-port", type=int, required=True)
    parser.add_argument("--video-port", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    run_case(args, "idle-peer", False)
    run_case(args, "tls-negotiation-peer", True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
