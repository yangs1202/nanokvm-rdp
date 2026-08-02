# NanoKVM RDP Gateway

[![Language: C](https://img.shields.io/badge/language-C11-00599C?logo=c&logoColor=white)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Build system: CMake](https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![Protocol: RDP](https://img.shields.io/badge/protocol-RDP-0078D4)](https://learn.microsoft.com/windows-server/remote/remote-desktop-services/clients/remote-desktop-protocol)
[![Video: H.264 over RTP](https://img.shields.io/badge/video-H.264%20over%20RTP-5C2D91)](https://datatracker.ietf.org/doc/html/rfc6184)

> A single-client remote desktop gateway for NanoKVM devices.

**Tags:** `nanokvm` · `rdp` · `remote-desktop` · `h264` · `rtp` · `udp` · `hid` · `ffmpeg` · `freerdp` · `c11` · `cmake`

`nanokvm-rdp` separates capture and RDP serving into two processes:

- **`nanokvm-agent`** runs on the NanoKVM device. It reads H.264 Annex-B frames from `libkvm.so`, packetizes them as RTP/H.264, and forwards RDP input as USB HID reports.
- **`nanokvm-rdp-gateway`** runs on a separate host. It receives the stream, decodes it with FFmpeg, and serves the resulting desktop through an RDP listener backed by FreeRDP.

The project supports one NanoKVM device and one connected RDP client at a time.

## Architecture

```text
RDP client
    │ TCP 3389 (TLS)
    ▼
nanokvm-rdp-gateway ───── TCP 3390 control ─────▶ nanokvm-agent
    │                                                    │
    │ ◀──────── UDP 5004 RTP/H.264 ──────────────────────┘
    ▼
RDP bitmap updates                                  NanoKVM capture + USB HID
```

## Transport and recovery

| Channel | Direction | Default port | Purpose |
| --- | --- | ---: | --- |
| RDP | Client → gateway | TCP `3389` | TLS-protected RDP session |
| Control | Agent → gateway | TCP `3390` | Input, stream state, heartbeat, and statistics |
| Video | Agent → gateway | UDP `5004` | RTP payload type 96 carrying H.264 |

The video sender uses a 1200-byte default MTU and supports both single-NAL packets and FU-A fragmentation. If the gateway detects an RTP sequence gap, it sends `IDR_REQUEST`; the agent drops P-frames until the next IDR frame.

The binary control protocol includes `HELLO`, `START_STREAM`, `STOP_STREAM`, `IDR_REQUEST`, `KEY`, `POINTER_ABS`, `POINTER_REL`, `WHEEL`, `RELEASE_ALL`, `PING`, `PONG`, `STATS`, and `ERROR`. The agent releases active HID state when the control connection ends, a stream stops, or the process exits.

## Prerequisites

- CMake 3.24 or later
- A C11 compiler and POSIX threads
- A FreeRDP source tree when building the gateway
- FFmpeg runtime libraries available to the gateway
- NanoKVM runtime libraries, including `libkvm.so`, available to the agent at runtime

## Build and test

Run local unit tests without building the device agent or gateway:

```sh
cmake -S . -B build/unit -G 'Unix Makefiles' \
  -DNANOKVM_RDP_BUILD_SERVER=OFF \
  -DNANOKVM_RDP_BUILD_AGENT=OFF
cmake --build build/unit --parallel 4
ctest --test-dir build/unit --output-on-failure
```

Build only the NanoKVM agent:

```sh
cmake -S . -B build/agent -G 'Unix Makefiles' \
  -DNANOKVM_RDP_BUILD_SERVER=OFF \
  -DNANOKVM_RDP_BUILD_TESTS=OFF
cmake --build build/agent --target nanokvm-agent --parallel 4
```

Build the gateway. `NANOKVM_RDP_FREERDP_DIR` must point to a FreeRDP source tree; it is deliberately not hard-coded in this repository.

```sh
cmake -S . -B build/gateway -G 'Unix Makefiles' \
  -DNANOKVM_RDP_FREERDP_DIR=/path/to/FreeRDP
cmake --build build/gateway --target nanokvm-rdp-gateway --parallel 4
```

For a cross-compiled gateway, also set `-DNANOKVM_RDP_OPENSSL_ROOT=/path/to/openssl-prefix` when needed by the toolchain.

## Run

Start the gateway with its defaults:

```sh
./build/gateway/nanokvm-rdp-gateway
```

Useful gateway options:

```text
-listen host:port  -cert file  -key file
-width n           -height n  -bitrate n
-control-port n    -video-port n  -swap-alt-command  -right-alt-as-hangul
-direct-gfx
```

`-swap-alt-command` exchanges `Alt` and `Command/GUI` scancodes, preserving left/right keys.
The option is disabled by default; when enabled, `AltGr` also exchanges with right `Command/GUI`.
`-right-alt-as-hangul` maps right `Alt/Option` to the HID Hangul/English key and takes precedence
over the right-side `Alt/Command` exchange.

Start the agent by supplying the gateway hostname or IPv4 address:

```sh
./build/agent/nanokvm-agent -gateway nanokvm-gw.yangs.sh \
  -control-port 3390 -video-port 5004 \
  -width 1920 -height 1080 -bitrate 3000
```

The gateway's default render size is 1920×1080 and its default bitrate is 3000. Use the same control and video port values on both sides.

## Deployment

Deployment scripts are provided in [`deploy/`](deploy/):

- [`S100nanokvm-agent`](deploy/S100nanokvm-agent) installs and manages the NanoKVM agent.
- [`S100nanokvm-rdp`](deploy/S100nanokvm-rdp) manages the legacy on-device RDP service.

Before starting the agent, update `GATEWAY`, `CONTROL_PORT`, `VIDEO_PORT`, `WIDTH`, `HEIGHT`, and `BITRATE` in `deploy/S100nanokvm-agent` for the target environment. Permit outbound traffic from NanoKVM to the gateway on TCP `3390` and UDP `5004`, and permit RDP clients to reach gateway TCP `3389`.

Do not run this agent simultaneously with the device's existing FoldVNC or stock KVM service: they share the NanoKVM capture path. The agent deployment script stops those services before starting the agent.

## Project layout

```text
src/       Gateway, agent, RTP/H.264, HID, and protocol implementation
tests/     Local unit and RTP loopback tests
deploy/    NanoKVM init scripts
cmake/     Toolchain configuration
tools/     Video decoder probes
```

## Security notes

- The RDP listener uses TLS. Provide a certificate and private key with `-cert` and `-key` in production.
- Network exposure is intentionally small but not authenticated by this repository's control protocol. Keep the control and video ports on a trusted network segment.
- This project is intended for controlled environments where the NanoKVM device and gateway are operated together.
