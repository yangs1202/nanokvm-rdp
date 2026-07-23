# NanoKVM RDP gateway

`nanokvm-agent`는 NanoKVM에서 `libkvm.so` H.264 Annex-B frame을 decode/re-encode 없이
RTP/H.264 UDP로 전송하고, gateway가 보낸 control message를 USB HID report로 적용한다.
`nanokvm-rdp-gateway`는 별도 호스트에서 TLS RDP listener, FFmpeg decode, RDP bitmap
encode를 실행한다. 단일 NanoKVM·단일 RDP client만 지원한다.

## Transport

- RDP: gateway TCP `3389`, self-signed TLS, NLA/RDP security 비활성화
- Control: agent가 gateway TCP `3390`으로 연결한다. length-prefixed binary protocol과
  `TCP_NODELAY`를 사용한다.
- Video: agent가 gateway UDP `5004`로 RTP payload type 96 H.264를 전송한다. 기본 MTU는
  1200 bytes이며 single NAL과 FU-A fragmentation을 지원한다.
- packet sequence gap은 gateway가 `IDR_REQUEST`를 보내고, agent는 다음 IDR 전까지 P frame을
  버린다.

Control message는 `HELLO`, `START_STREAM`, `STOP_STREAM`, `IDR_REQUEST`, `KEY`,
`POINTER_ABS`, `POINTER_REL`, `WHEEL`, `RELEASE_ALL`, `PING/PONG`, `STATS`, `ERROR`다.
agent는 control disconnect, `STOP_STREAM`, process 종료에서 `ReleaseAll`을 실행한다.

## Build and test

```sh
cmake -S . -B build/unit -G 'Unix Makefiles' \
  -DNANOKVM_RDP_BUILD_SERVER=OFF -DNANOKVM_RDP_BUILD_AGENT=OFF
cmake --build build/unit --parallel 4
ctest --test-dir build/unit --output-on-failure

BUILD_DIR="$PWD/build/riscv64-agent" ./scripts/build-riscv64.sh
```

gateway는 Linux x86_64에서 FreeRDP server development dependency 또는 source tree를 명시해서
빌드한다. 경로는 저장소에 하드코딩하지 않는다.

```sh
cmake -S . -B build/gateway -G 'Unix Makefiles' \
  -DNANOKVM_RDP_FREERDP_DIR=/path/to/FreeRDP
cmake --build build/gateway --target nanokvm-rdp-gateway --parallel 4
```

## Deployment

장비의 기존 RDP/FoldVNC service와 capture path를 동시에 실행하면 안 된다. 배포 전에 아래를
백업하고, 문제 발생 시 복구 명령으로 기존 service를 되돌린다.

```sh
ssh root@10.97.12.49 'ts=$(date +%Y%m%d%H%M%S); mkdir -p /data/nanokvm-rdp-backup/$ts; cp -a /root/nanokvm-rdp /data/nanokvm-rdp-backup/$ts/; cp -a /etc/init.d/S100nanokvm-rdp /data/nanokvm-rdp-backup/$ts/'

# 복구
ssh root@10.97.12.49 '/etc/init.d/S100nanokvm-agent stop || true; /etc/init.d/S100nanokvm-rdp start'
```

`deploy/S100nanokvm-agent`의 `GATEWAY`와 포트를 대상 host에 맞춰 배포한다. 방화벽은
NanoKVM → gateway TCP `3390`, UDP `5004` outbound를 허용해야 한다. gateway `3389`은
Jump Desktop이 접근할 수 있어야 한다.
