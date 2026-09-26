# Browser latency-target handoff

## 회사 Mac에서 브라우저로 접속

대상 회사 Mac에서 http://10.97.11.124:48761/ 에 접속한다. SSH, Codex, 설치는 필요 없다. 이 주소는 메인 Mac의 임시 내부 서버이며 서버 실행과 내부망 경로가 유지되어야 한다. Start session 후 ELAPSED MILLISECONDS와 UPDATE 번호가 증가한다. 원본 화면과 Windows App의 원격 화면을 같은 카메라 프레임에 담아 타이머 값을 비교한다. Stop 또는 탭 숨김은 타이머를 멈춘다. 다시 Start하면 기록과 타이머를 초기화하므로 먼저 Export한다.

인수 기준은 네트워크 transit 제외, 측정 오차 상한 포함 80ms 미만이다. 타이머 차는 직접적으로 display-to-display 지연이다. 원본 패널 표시와 NanoKVM 캡처의 차이, 화면 갱신/scanout, 촬영 노출/rolling shutter 및 판독 오차를 근거 있는 상한으로 포함해야 capture-to-presentation 인수에 사용할 수 있다. ms 숫자만으로 1ms 정밀도를 주장하지 않는다. 네트워크 포함 상한이 80ms 미만이면 차감 없이 보수적으로 통과 가능하다. 초과하면 네트워크 제외 기준 판정은 추가 측정이 필요하다. 입력 경로는 별도로 검증한다.

아래 Codex 전달 절차는 선택 사항이다.

## 다른 Mac의 Codex에 전달할 요청

아래 요청을 그대로 전달한다. SSH나 같은 Codex 계정은 필요하지 않다.

```text
nanokvm-rdp의 원격 입출력 지연 측정을 준비해주세요.
저장소: https://github.com/yangs1202/nanokvm-rdp
PR: https://github.com/yangs1202/nanokvm-rdp/pull/1
브랜치: codex/go-gateway-refactor

기존 작업 파일을 덮어쓰지 말고 별도 디렉터리에서 이 브랜치를 받으세요.
먼저 이 Mac이 NanoKVM HDMI/USB에 연결된 대상 Mac인지,
Windows App을 실행하는 클라이언트 Mac인지 사용자와 확인하세요.
대상 Mac이면 docs/latency-target-handoff.md를 읽고
tools/latency-target.html을 브라우저로 열어주세요.
설치나 SSH 설정 변경은 필요하지 않습니다.

macOS/브라우저 버전, 화면 해상도와 주사율, 양쪽 Mac의 역할을 기록하세요.
사용자가 Start session을 누른 뒤 Windows App을 통해 테스트 영역에
키 down/up, 포인터, 버튼, 휠을 입력하도록 안내하고,
Stop session → Export JSON 결과를 보관해주세요.
로컬에서 직접 넣은 입력은 원격 입력 표본과 구분하세요.

현재 nanokvm-gw.yangs.sh는 기존 C 게이트웨이일 수 있으므로
Go 후보 버전의 배포 커밋/이미지가 확인되기 전에는 Go 성능으로 보고하지 마세요.
HTML 기록은 대상 브라우저 도착과 DOM 변경만 보여줍니다.
실제 Windows App 표시/OS 반영 전체 지연이나 80ms 통과를 주장하지 마세요.
실제 표시를 관측할 동시 촬영 또는 검증된 계측 방법이 있는지도 확인하세요.
다른 장비의 performance.now 값을 직접 빼거나 RTT/2를 차감하지 마세요.

환경 정보, JSON 파일 위치와 측정 방법/미측정 구간을 요약해 사용자에게
이 메인 Codex 태스크로 전달하도록 해주세요. 자동 배포는 하지 마세요.
```

## Local helper verification

The main task exercised Start, key down/up, Tab focus traversal, Stop and JSON download in a browser. The export contained event kinds/phases and visual sequence numbers without key contents. Synthetic pointer/wheel events while unfocused did not change the count; 2050 focused pointer events plus session start retained 2048 records and reported three drops. After Stop, a synthetic key event neither changed the count nor prevented its default action. These checks validate the helper only, not physical remote latency.

tools/latency-target.html is a target-side observation aid for a machine that cannot be reached over SSH. It is deliberately self-contained: copy the file to the target Mac, open it in a browser, and use the browser's normal file-open flow. It makes no network requests, registers no service worker, collects no global keyboard or pointer events, and sends no input automatically.

## Who runs what

The target operator opens the HTML on the target Mac and owns the focused test area. The source/client operator owns the capture or RDP client side. Confirm both roles before a run. The page's visual-marker and missing-marker buttons are explicit operator annotations; they are not a transport from the source side and do not turn a browser repaint into a presentation timestamp.

The page shows a large DOM visual-marker sequence and a high-contrast changing pattern. Start a session explicitly, focus the test area, and use the Advance visual marker button for an agreed local DOM update. Use Mark missing when an expected marker is known to be absent. Stop explicitly and export the bounded in-memory JSON record. Keyboard events are recorded only inside the focused area and retain phase/kind/sequence/timestamp, never key values or key codes. Pointer, button, and wheel events are recorded at the same browser handler boundary. Each recorded input also advances the visible DOM marker, updates the visible phase indicator, and includes that marker sequence in the event.

## Handoff procedure

1. The other-account Codex opens the tool from the repository PR 1 checkout and transfers tools/latency-target.html to the target Mac using an approved offline method.
2. On the target Mac, open the file in the browser that will be used for the observation. Keep the page visible in the intended client/display arrangement and verify that the large sequence marker is legible.
3. Confirm the target/client roles, the run identifier, the expected visual sequence, and the deployed gateway version. The current production baseline is the C gateway; do not treat the Go candidate as the deployment target until the actual binary/image under test is explicitly confirmed.
4. Click Start session. Click inside the test area before exercising keyboard, pointer, button, or wheel input. The tool records only while the session is active and the area is focused.
5. For visual observations, advance or annotate the agreed DOM markers with Advance visual marker and record known gaps with Mark missing. For input observations, exercise only the agreed controls and keep the target application/handler context documented separately.
6. Click Stop session, then Export JSON. Preserve the JSON with the run identifier and the independent source/client evidence.

## What this can and cannot prove

The JSON timestamps use the target browser's performance.now() clock. They describe arrival at the browser application-handler boundary, not the earliest HID arrival and not the moment the target OS application visibly applies the input. Browser requestAnimationFrame requests and callbacks are not proof of actual display presentation; this tool does not use them as proof.

For strict acceptance, measure each paired event externally:

- screen capture → actual client presentation
- input occurrence → target OS application effect

The target page alone cannot supply either source timestamp or actual physical presentation timestamp. Windows App actual presentation requires separate simultaneous high-speed capture or a presentation measurement method validated for that client. Do not subtract clocks from different devices directly, and do not estimate network time by subtracting RTT/2. Report p50, p95, p99, max, missing frames/events, and failures; averages alone are not sufficient.

The tool does not calculate a 80 ms pass/fail result. overflowCount and droppedEventCount identify bounded-buffer loss; missingMarkersMarked and missing-marker records identify operator-observed gaps. Interpret those records together with the independent capture/presentation and OS-application evidence.

## Safety and limits

- No global key listener is installed, and no key content is stored.
- No automatic input, network transport, WebSocket, service worker, or remote-control path exists.
- The event buffer is bounded to 2048 records. Overflow drops the oldest records and is exported as counters.
- A fresh session resets the in-memory run ledger. Export before closing or reloading the page.
- This is a measurement handoff aid, not a production health check or a substitute for validating the deployed gateway version.
