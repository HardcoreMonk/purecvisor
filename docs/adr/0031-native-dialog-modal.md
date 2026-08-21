# ADR-0031: 웹 표준 native `<dialog>` 모달 (PCV.modalCore)

날짜: 2026-07-20
상태: accepted

## 맥락
Web UI 모달은 두 갈래의 손코드 오버레이였다:

- **시스템 A** — `showModal(body)`/`closeModal()` (ui.js). 정적 `#mbg`/`#mc` div + `.hidden` 토글, `_modalStack`(childNodes save/restore)로 중첩, app.js F-2 블록의 수동 ESC/Tab focus-trap. **154 호출부**. 이 시스템 자체엔 focus-trap이 없었고 ESC/Tab은 app.js에 분리돼 있었다.
- **시스템 B** — `Modal.show({...})` (modal.js). 동적 `#pcv-modal-overlay` + 자체 `_ensureOverlay`/`_trapFocus`/keydown. **12 호출부**.

focus-trap·ESC·백드롭·top-layer·배경 inert를 전부 손으로 관리 → 중복, 누락(시스템 A focus-trap 부재), z-index 관리 부담. `<dialog>` 요소는 Baseline 2022(전 에버그린 지원)로 이 모든 것을 native 제공한다.

vol2 프론트 개선 항목 **B1**(웹 표준 UI 프리미티브 채택).

## 결정
단일 엔진 **`PCV.modalCore`**(`ui/modules/modal-core.js`)가 native `<dialog>` 라이프사이클을 소유하고, 두 공개 API가 모두 이 엔진에 위임한다.

- 열 때마다 `<dialog class="modal">`를 동적 생성해 `dialog.showModal()`로 **top-layer**에 띄운다.
- **focus-trap·ESC·`::backdrop`·배경 inert·top-layer 스택 = native.** 중첩은 native 다중-`<dialog>`(부모는 DOM에 남아 inert → 입력 상태 자동 보존) — 수동 `_modalStack` **폐지**.
- **공개 계약 불변**: `showModal(h,opts)`·`closeModal(force)`·`Modal.show({...})`·`Modal.close()`·`Modal.setMessage()` (166 호출부 무수정).
- `#mc` 직접 접근 사이트는 `PCV.modalCore.currentBody()`/`currentDialog()`로 이관.
- **bare 변형**(`openBare`, phase 2): 카드 없는 오버레이(cmd-palette·kbd-help·iso-browser — global-search 는 R4 ⌘K 통합 검색에 흡수돼 소멸, 현재 소비자 3곳)를 `openBare(node, {dialogClass, onClose})`로 같은 엔진에 태운다(`.modal` 카드/`.modal-body`/width/`id=mc` 래핑 없음, `contentEl`이 곧 `<dialog>` 자식). `open`/`openBare` 공유 리스너(백드롭·`cancel`·out-of-band close)는 `_wireDialog`로 추출.

## 규약 (신규 UI 코드)
- 모달은 `showModal(node)` 또는 `Modal.show({body:node})`. **body는 Node/Node배열**(HTML 문자열 금지 — DOM-safe [ADR-013(zone)]).
- 손수 오버레이 div + 수동 ESC/focus-trap 배선 **금지** — modalCore 경유.
- 접근성 이름: 본문 첫 제목(h1~h3) → `aria-labelledby` 자동, 없으면 generic `aria-label="Dialog"`.
- **오버레이(카드 없음)**: `PCV.modalCore.openBare(node, {dialogClass, onClose})`. 닫기는 `openBare` 반환 dialog ref의 `.close()` — **`modalCore.close(false)`(최상위 닫기) 금지**(중첩·`action();close` 시 엉뚱한 dialog가 닫힌다). 명시 닫기 직후 같은 콜스택에서 다른 스택 연산(예: `showModal` replace → `_closeTop`)이 이어지면 `PCV.modalCore.closeDialog(dlg)`(특정 dialog 동기 스택 제거)를 쓴다 — iso 브라우저가 이 경우(좀비 모달 방지).
- **상태 플래그 동기화**: 오버레이 open/close 플래그는 `onClose` 콜백에서 리셋(native ESC·백드롭·명시 닫기 전 경로 커버). 단 `onClose`는 `if (xDialog !== mine) return` 가드로 **현재 dialog일 때만** 리셋 — 동일-틱 재토글 시 옛 dialog의 비동기 close가 새 상태를 덮어쓰는 것을 막는다.

## 결과
- 손코드 대거 제거: `_modalStack`·`_ensureOverlay`/`_trapFocus`·정적 `#mbg`/`#mc` 마크업·app.js F-2 ESC/Tab·ui.js 중복 focus-trap·dead `bindGlobalModalEsc`·`.modal-bg` CSS. (소스 순 −9줄 리팩터, `lint:domsafe` 9→8)
- **접근성 향상**: 시스템 A가 native focus-trap 획득. z-index 관리 소거.
- **트레이드오프**: `<dialog>`는 top-layer라 토스트(z-index)가 모달 열림 중 가려진다. 삭제 진행 플로우는 모달 내부에 결과를 표시한 뒤 닫고 토스트는 닫힘 뒤 노출되므로 **정보 손실 없음**(허용 결정). 토스트 top-layer 전환은 별도 후속.
- **브라우저**: 에버그린 전용(폴리필 없음).
- **phase 2 (오버레이)**: cmd-palette·kbd-help·global-search·iso-browser → native `<dialog>`. `app.js` ESC 분기 4개 제거(native ESC 대체, zen/notif는 잔존), 구 풀뷰포트 wrapper div·백드롭 rgba 제거(→`::backdrop`). 신설: `openBare`·`_wireDialog`·`closeDialog`. 실브라우저 검증에서 2건 수정 — ① 전역 `* { margin:0 }`이 native dialog UA `margin:auto`를 지워 발생한 오버레이 좌측정렬 회귀(`dialog.pcv-dialog { inset:0; margin:auto }` 복원), ② iso 선택→`renderWiz` replace 시 iso의 비동기 close가 스택에 잔류해 `_closeTop`이 wizard를 고아로 만든 좀비 모달(→`closeDialog` 동기 제거). `lint:domsafe` 8 유지(불변).

## 비모달 패널: Popover API
모달 `<dialog>`(`showModal` — 배경 inert + 백드롭 강제)은 **비모달** 패널(알림 센터처럼 배경 상호작용을 유지해야 하는 드롭다운)엔 부적합하다. notif-center(우상단 드롭다운)는 웹 표준 **Popover API**(`popover="auto"` + `showPopover()`)로 전환 — top-layer·native ESC·바깥클릭 light-dismiss는 native 제공하되 **백드롭/inert 없음**(비모달 유지). **`modalCore`(모달 전용) 경유 안 함** — Popover는 별개 프리미티브. `toggle` 이벤트로 상태 플래그 동기화 + 요소 정리, mark/clear는 제자리 재렌더(구 close+reopen 처닝·race 제거). 닫힘 상태는 `.notif-center:not(:popover-open){display:none}`로 명시(저자 `display:flex`가 UA `[popover]:not(:popover-open){display:none}`를 이겨 발생하는 1프레임 유령 방지). **zen-mode는 `body.zen-mode` 뷰모드(오버레이 아님)라 제외.** Popover API = Baseline 2024.

**규약(신규 UI)**: 모달 = `modalCore`(native `<dialog>`). **비모달 dismissable 패널 = Popover API**(`popover="auto"` + `showPopover()`). 손수 `z-index` 드롭다운/오버레이 금지.

## 관련
- phase 1: 표준 `<dialog>` 기반 모달 코어.
- phase 2: `openBare`/`closeDialog` 오버레이 통합.
- 비모달 패널: 알림 센터를 Popover API로 분리.
- 전제: DOM-safe [ADR-013(zone)], 프론트 IIFE 모듈 [ADR-0013].
