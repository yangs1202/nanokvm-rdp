# 영상 성능 벤치마크와 리팩토링 승인 기준

## 성능 요구사항

**리팩토링 이후 영상 성능은 리팩토링 이전과 동일하거나 좋아야 한다.**
같은 하드웨어·영상 품질·프로토콜·부하에서 지연이 증가하거나 화면 전달률이 낮아지는
변경은 성능 요구사항을 충족하지 않는다. 화질·해상도·bitrate를 낮춰 얻은 지연 감소를
리팩토링 성능 개선으로 인정하지 않는다. 측정 오차나 자료 부족은 통과 사유가 아니다.

## 보존한 최초 기준선

| 항목 | 값 |
| --- | --- |
| ID | 2026-09-29-video-latency |
| 상태 | preliminary — 초기 관측 기준선, 정밀 회귀 승인용으로 불충분 |
| 경로 | Windows 원본 → NanoKVM → gateway → Mac Windows App |
| 측정량 | 수신 화면에 보이는 원본 draw timestamp의 나이 |
| 표본 | 약 41.5초에 걸친 20개, 연속·균일 샘플링 아님 |
| p50 추정 | 128.5ms; 계산상 범위 88.8~171.2ms |
| 표본 p95 추정 | 160.0ms; 계산상 범위 121.8~225.2ms |
| 표본 최대 추정 | 180.5ms; 계산상 범위 135.8~238.2ms |
| 원본 시계 보정 | ±3.70ms, 수집 후 drift 미측정 |
| 캡처 호출 폭 | 중앙값 68.5ms, 최대 216ms |

[측정 보고서](2026-09-29-video-latency.md),
[원시 CSV](2026-09-29-video-latency.csv),
[계산 JSON](2026-09-29-video-latency.json),
[메타데이터와 SHA-256](2026-09-29-video-latency.manifest.json)을 함께 보존한다.
원본 스크린샷은 해당 채팅 도구 출력에만 있고 저장소에는 없다.
기존 파일을 새 측정으로 덮어쓰지 않는다. 수정이 필요하면 원본을 유지하고 별도 정정본을 만든다.

현재 자료는 약 0.1~0.2초 규모의 첫 기준선이다. 실제 배포 바이너리·협상 codec 등의
조건이 미확인이므로 이후 측정값이 128.5ms 이하라는 이유만으로 성능 통과를 선언하지 않는다.

## 현재 방법 재현

1. Mac에서 저장소 루트 기준으로 서버를 실행한다.

   ```sh
   python3 tools/video-latency/server.py --bind <Mac-LAN-IP>
   ```

2. NanoKVM에 HDMI로 연결된 원본 Windows의 브라우저에서
   `http://<Mac-LAN-IP>:8765/1`을 연다. **Start / resync**를 누르고 페이지를 보이게 둔다.
3. Mac Windows App으로 gateway에 연결한다. 화면의 FRAME 값이 진행하는지 확인한다.
   소스/클라이언트가 같은 페이지에 직접 접속한 두 화면을 비교하는 것이 아니다.
4. 수신 Windows App 스크린샷을 찍고, 화면 timestamp·FRAME·SYNC와 캡처 호출 전후
   시각을 기록한다. Computer Use에서는 같은 도구 실행 내에서 다음과 같이 수집한다.

   ```javascript
   const before = Date.now();
   await app.getScreenshot();
   const after = Date.now();
   nodeRepl.write({ before, after });
   ```

5. 화면 값을 CSV로 전사하고 숫자를 재확인한다. 단위는 ms다.

   ```csv
   source_ms,capture_before_ms,capture_after_ms,sync_error_ms,frame_id
   ```

6. 분석 결과를 새로운 실행 ID로 저장한다.

   ```sh
   python3 tools/video-latency/analyze.py docs/measurements/<run-id>.csv > docs/measurements/<run-id>.json
   ```

7. 서버와 캡처 시계의 차이, 보정 후 경과 시간, 누락·판독 실패 및 모든 제외 사유를
   보고서에 적는다. 서버와 Windows App을 사용한 뒤 정상 종료한다.

계산식, clock offset의 구간 유도 및 도구 제약은
[도구 문서](../../tools/video-latency/README.md)에 있다. Date.now()와 서버 monotonic
기반 시계 사이의 clock step이 있으면 해당 실행을 무효 처리한다. NTP 서버
`ntp.yangs.sh`는 보조 기준이며 직접 보정 결과를 대신하지 않는다.

## 리팩토링 전후 정식 비교 절차

현재 방법의 불확실성을 줄인 **동일 수집기**로 변경 전과 변경 후를 모두 측정한다.
기존 20개 자료는 그대로 유지하고, 구현 변경 전에 정식 baseline을 추가한다.
수집기를 바꾼 결과와 기존 Computer Use 결과를 동일 정확도의 숫자로 비교하지 않는다.

### Formal collector 계약

정식 실행은 [collector.py](../../tools/video-latency/collector.py)와
[입력·권한 문서](../../tools/video-latency/collector-README.md)를 사용한다.
이는 기존 `server.py`의 `/1/clock` 응답으로 capture producer의
`time.monotonic_ns()`를 같은 server monotonic timeline으로 보정하고, 동시에
`/1?run_id=<run-id>` source page의 browser clock 보정을 collector가 시작/종료에
자동 요청한다. 두 시계의 시작/종료 offset·uncertainty·raw samples와 원시 JSONL
관측, capture 전후 시각, first-seen frame ID, source drift/bracketing 상태를 단일
collector JSON에 남긴다. Mac capture-host `sync_error_ms`, source-window clock
uncertainty, end drift는 서로 다른 값이며 합치거나 대체하지 않는다. 실행별
`--metadata` 입력은 [formal manifest 템플릿](2026-09-29-video-latency-formal-manifest.json)에서
복사한다. 이 입력의 `source`, `video`, `client`, `network`, `formal_measurement`는 모두
top-level fields이며, `collector.py`가 변경 없이 결과 collector artifact의 `metadata` 아래에
보존한다. 따라서 analyzer가 읽는 경로는 `artifact.metadata.formal_measurement`다.

정식 수집에는 저장소의 [ScreenCaptureKit capture producer](../../tools/video-latency/capture-producer/README.md)를
사용한다. 이 macOS 14+ 도구는 receiving Windows App 창 하나를 `--window-id`로 선택하고,
각 PNG에서 Vision OCR로 source timestamp/FRAME을 읽어 `collector.py` JSONL 계약에 맞춰
출력한다. `--output-dir`에는 PNG·OCR sidecar와 `session.json`을 보존한다. 먼저
`swift run capture-producer --preflight`, 이어 `--list-windows`를 실행하고, Screen Recording
권한을 얻은 실행 host에서 정확한 window ID를 사용한다. wall-clock, 도구 응답 도착 시각,
캐시 가능성이 있는 screenshot의 미문서 timestamp는 formal 입력이 아니다.

```sh
swift run capture-producer \
  --window-id <receiving-windows-app-window-id> \
  --output-dir /absolute/path/to/<run-id>-captures \
  --count 1000 --interval-ms 33 | \
python3 tools/video-latency/collector.py \
  --server-url http://<capture-host-LAN-IP>:8765/1 \
  --metadata docs/measurements/<run-id>-metadata.json \
  --run-id <run-id> --output docs/measurements/<run-id>-collector.json

python3 tools/video-latency/analyze.py \
  --collector-artifact docs/measurements/<run-id>-collector.json \
  --producer-session /absolute/path/to/<run-id>-captures/session.json \
  > docs/measurements/<run-id>-analysis.json
```

`--count 1000 --interval-ms 33`은 약 33초라는 **명목값**일 뿐이다. 분석기는 capture
before/after의 실제 관측 elapsed wall-clock가 60초 이상인지 별도로 검사하므로, 이 명령만으로
정식 run이 되지 않는다. source/receiver를 최소 30초 워밍업한 뒤 실제 관측 기간이 60초 이상이
되도록 count/interval을 늘리고, metadata의 `formal_measurement.warmup_elapsed_ms`에 실제 값을
기록한다. metadata에는 모든 source/video/client/network 조건과 `condition_id`, `pattern_version`,
`workload`, `capture_timestamp_semantics_verified: true`, source/receiver/capture geometry,
concrete `capture_settings`, `video_quality_verified: true`, 실제 GOP(`actual_gop`)도 채운다.
`network.contention: false`처럼 확인된 false/zero 값은 unknown으로 바꾸지 않는다.
`session.json`의 reconciled attempt count, Vision OCR 실패 수와 artifact 경로도 분석 결과에 남는다.
OCR failure가 하나라도 있으면 missing observation을 보정하거나 추측하지 않으며 해당 formal run은
ineligible이다.

20장 preliminary CSV/JSON/MD/manifest는 historical baseline으로만 보존한다. 새 collector
JSON은 그것을 덮어쓰거나 합산하지 않으며, formal A/B 판정 전에 같은 collector·동일 품질
조건에서 얻은 A와 B만 분석한다.

- source clock offset·불확실성을 실행 전후 수치로 저장한다. `collector.py`는
  browser `/1/clock` raw request/response, 선택 offset·uncertainty와 run ID를
  `source_clock_calibration`에, capture-host 보정을 `calibration_start`/
  `calibration_end`에 별도로 export한다. `analyze.py --collector-artifact`는 어느
  시계든 시작/종료 보정 누락, 불명확한 uncertainty/drift, drift 검출 후 미보정,
  source clock이 capture window를 bracket하지 못한 실행을 **non-eligible**로
  표시한다. 이것은 지연 또는 A/B 통과 판정이 아니다.
- 연속 수집기는 저장소의 ScreenCaptureKit producer가 제공하는 실제 capture timestamp와
  프레임 식별값을 저장한다. `collector.py`는 이 JSONL 기록과 first-seen 계산을 구현하며,
  producer의 권한·window ID·sidecar/session 보존 규칙은 위 명령과 producer 문서를 따른다.
- 초기 워밍업 30초 후 실제 elapsed wall-clock 60초 이상 수집하고, 한 조건당 유효 표본
  1,000개 이상을 **필수**로 한다. nominal count/interval은 이를 대체하지 않는다.
  동일 프레임 반복은 age 계산에 유지하고, FPS 계산은 별도로 첫 관측을 사용한다.
- 최소 세 쌍의 A/B 실행을 수행한다. A는 변경 전, B는 변경 후이며 순서를 교대한다.
  이 횟수는 시작 기준일 뿐이다. 변동이 크거나 판정이 겹치면 추가 측정한다.
- 동일 timestamp 패턴, 정지 후 마지막 화면 업데이트, 별도 고정된 움직임 workload를
  각각 비교한다. pattern 버전과 실제 갱신률을 기록한다.
- bitmap/Progressive와 AVC420은 별도 조건으로 비교한다. 새 Web/VNC 결과는 RDP 회귀
  통과를 대신하지 않는다. 관찰자 추가 등 새 부하는 별도 확장 벤치마크로 남긴다.

각 실행에 반드시 남길 조건:

| 분류 | 기록할 값 |
| --- | --- |
| 코드·실행물 | Git revision, dirty patch 또는 해시, 실제 gateway image digest/바이너리 SHA, agent 바이너리 SHA |
| 원본·장치 | Windows/브라우저 버전, 디스플레이 해상도·refresh·DPI, NanoKVM 모델/펌웨어 |
| 영상 | 실제 codec, agent 해상도·bitrate·GOP·FPS, gateway render 크기와 옵션 |
| 수신 | Mac/macOS 및 Windows App 버전, 창·화면 크기, scaling, 모니터 refresh |
| 네트워크 | source/agent/gateway/client 경로, 유무선, RTT·손실·경합 여부 |
| 계측 | pattern/수집기/분석기 해시, 샘플 주기, 원시 시각, 보정 전후 offset와 오차 |
| 결과 | 지연 p50/p95, 충분한 표본의 p99, 실제 FPS·정지 구간·drop, CPU/RSS, 오류 |

미확인 값은 null/unknown으로 남기고 추정값으로 채우지 않는다. 비교에 필요한 조건이
미확인이면 판정은 보류다. 캡처 부하 자체도 A와 B에서 동일하게 유지한다.

## 판정 규칙

- **목표:** 지연 p50/p95가 기준선보다 증가하지 않고, 동일 품질에서 전달 FPS가 감소하지
  않아야 한다. 긴 화면 정지·마지막 프레임 유실·입력 고착 같은 기능 회귀가 없어야 한다.
  충분한 표본으로 수집한 p99와 자원 사용량도 악화 여부를 함께 검토한다.
- **개선 확인:** 반복 A/B의 계측 오차·실행 변동을 포함한 차이 구간에서 B−A의 상한이
  지연 지표 모두 0 이하이고, 다른 필수 지표도 유지되면 개선/유지로 인정한다.
- **동등성:** 엄밀한 0ms 차이는 유한 측정으로 증명하기 어렵다. 비교 전에 수집기 자체의
  반복 측정 오차를 산정해 고정하고, 관측 차이가 그 해상도 안에 있으며 반복 실행에서
  일관된 악화가 없으면 '측정 해상도 내 동등'으로만 보고한다. 오차를 사후에 키우거나
  예전 계획의 10% 성능 저하를 허용 기준으로 사용하지 않는다.
- **판정 보류:** 단순 구간 겹침, 넓은 오차, 조건 불일치, 표본 부족은 자동 통과가 아니다.
  더 정확한 수집/추가 A/B로 해결하며, 해결 전에는 성능 요구 충족을 선언하지 않는다.
- **회귀:** 반복 측정에서 오차를 넘어선 악화가 확인되면 수정하거나 해당 변경을 되돌린다.

기존 20-sample CSV/JSON은 preliminary historical baseline 및 fixture arithmetic validation일
뿐이며 formal analysis 또는 PASS에 사용할 수 없다. Eligible collector artifact에 대해서만
`analyze.py`는 p50/p95/p99의 capture/source-clock·capture-window error bounds, observed
first-seen FPS(관측 전체 시작/끝 경계의 정지도 포함하지만 전달/표시 FPS가 아니라는 제한 포함),
drop/stall/resource/error fields를 계산한다.
실제 session/metadata/조건/quality가 하나라도 모호하면 `inconclusive`/`not_computed`가 된다.
A/B는 `--ab-pair A.json B.json`을 최소 세 번 주고, 모든 pair의 완전하고 일치하는 조건 metadata가
있을 때만 계산하며 자동 PASS/FAIL 판정은 하지 않는다.
