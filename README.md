# NanoKVM RDP bridge

`nanokvm-rdp`는 NanoKVM의 `libkvm.so` H.264 Annex-B output을 FreeRDP server-side
RDPGFX AVC420으로 그대로 전송하고, 표준 RDP input을 USB HID gadget으로 변환한다.

## 빌드

로컬 unit test:

```sh
cmake -S . -B build/unit -DNANOKVM_RDP_BUILD_SERVER=OFF
cmake --build build/unit --parallel 4
ctest --test-dir build/unit --output-on-failure
```

`scripts/build-riscv64.sh`는 Zig riscv64-musl compiler, vendored FreeRDP와
riscv64 OpenSSL prefix가 필요하다. 현재 검증된 OpenSSL source는 3.1.4이며 SHA-256은
`840af5366ab9b522bde525826be3ef0fb0af81c6a9ebd84caa600fea1731eee3`이다.

```sh
OPENSSL_ROOT="$PWD/build/openssl-riscv64-v2" ./scripts/build-riscv64.sh
```

## 장비 서비스

`deploy/S100nanokvm-rdp`는 다음 동작을 의도한다.

- 실행 전 기존 `/etc/init.d/S99foldvnc`를 중지해 `libkvm.so` capture 충돌을 방지한다.
- `/root/nanokvm-rdp/cert.pem` 및 `key.pem`가 없으면 장비 OpenSSL로 self-signed TLS
  certificate를 생성한다.
- stop 시에는 모든 HID report를 release한 뒤 FoldVNC를 다시 기동한다.

### 보안 제약

TLS만 허용하며 RDP Security와 NLA는 비활성화했다. 이 NanoKVM에는 NLA가 사용할
계정 검증 backend가 없고, RDP client 호환성을 위해 AVC420-only PoC를 먼저 제공한다.
따라서 `:3389`은 신뢰된 관리망에서만 노출해야 하며, 인터넷 또는 비신뢰 LAN에 직접
공개하면 안 된다. 인증서 fingerprint를 RDP client에서 확인해야 한다.

AVC420/RDPGFX를 광고하지 않는 client는 연결을 명시적으로 종료한다. bitmap fallback은
1080p decode/re-encode 비용 때문에 이 범위에 포함하지 않는다.
