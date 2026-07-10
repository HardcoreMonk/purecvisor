---
name: verify
description: purecvisor-single UI 변경 실브라우저 검증 레시피 — 목 API 서버 + Playwright 키보드/마우스 구동
---

# UI 변경 검증 레시피

프론트엔드(ui/) 변경을 데몬·자격증명 없이 실브라우저로 검증하는 절차.

## 핸들: 목 API 서버

로컬/서버 데몬을 건드리지 않는다 (설치 경로 `/usr/local/share/purecvisor/ui` 덮어쓰기·자격증명 추출은 auto 모드에서 차단됨). 대신 워크트리 `ui/`를 정적 서빙하고 `/api/*`를 전면 목으로 응답하는 단일 python 서버를 스크래치패드에 띄운다:

- `POST /api/v1/auth/token` → `{access_token: <더미 JWT — header.payload.sig 형식, exp 충분히 미래>}`
- `GET /api/v1/vms` → `{data:[{name,state:'running'|'shutoff',vcpu,memory_mb}, ...]}` (j/k 탐색용 3~4대)
- `GET /api/v1/auth/whoami` → `{data:{username,role:'ADMIN'}}` (role 가시성 #15 — 전 메뉴 노출)
- 그 외 → `200 {"data":[]}` (UI는 실패 허용 설계라 충분)

로그인은 아무 더미 자격증명으로 폼 로그인(`#login-user`/`#login-pass` → Enter). WS(`/api/v1/ws/events`)는 핸드셰이크 실패 콘솔 에러가 나지만 무해.

## 상태 읽기

- **`window.PCV.state`** (라이브 getter) — `selectedVmIndex`/`currentTab`/`vmList`는 반드시 이걸로 읽는다. `window.selectedVmIndex` 등 직접 읽기는 stale.
- 메뉴 상태: `.menu-item.open`, `aria-expanded`, roving 탭 스톱은 `tabindex="0"` 위치.

## Playwright 함정 (실측)

1. **입력 사멸**: `page.screenshot()`·`waitForTimeout()` 직후, 그리고 MCP 도구 호출 사이에 CDP 실키/클릭 전달이 죽는다 (evaluate는 계속 동작 — 상태만 바뀌고 입력만 무효). **모든 키 시나리오를 단일 `browser_run_code_unsafe` 호출에서, 사이에 waitForTimeout/screenshot 없이 연속 실행**한다. 스크린샷은 마지막에만. 죽으면 `context().newPage()`로 새 페이지에서 재개(goto 직후엔 살아 있음).
2. **beforeunload 다이얼로그**: 앱이 beforeunload를 등록하므로 `page.reload()`가 다이얼로그에 막힌다. `p.on('dialog', d => d.accept())`를 미리 걸거나 reload 대신 newPage를 쓴다.
3. 입력 도달 여부 판별: document capture 트레이서(`window.__t.push({k:e.key, dp:e.defaultPrevented})`)로 "미도달"과 "도달했지만 앱이 무시"를 구분한다.

## 커버할 플로우 (키보드 트랙)

메뉴바 roving(←/→ 순환·Enter 열기·↑/↓ 순환·드롭다운 내 ←/→ 인접 메뉴·Esc), .mi Enter 활성화(3-3 위임) 후 currentTab 가로채기 없음, VM 목록 j/k + body Enter(F1), 메뉴바 Tab 1회 탈출, activity 아이콘 Enter/Space.
