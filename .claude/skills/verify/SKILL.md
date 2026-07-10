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
4. **evaluate 컨텍스트 파괴 재시도**: 로그인 직후·`navigateTo()` 직후 evaluate가 해시 전환과 경합해 "Execution context was destroyed"로 죽는다. 모든 evaluate를 재시도 래퍼(파괴 메시지면 400ms 대기 후 최대 4회)로 감싼다. `navigateTo`는 `p.evaluate(n => navigateTo(n), tab).catch(파괴무시)` 후 `waitForTimeout(350)`.
5. `browser_run_code_unsafe` 안에 `require` 없음 — 파일 저장 불가. 대량 데이터(골든 스냅샷 등)는 앱 오리진 `localStorage`에 저장하고 비교도 브라우저 안에서 수행해 요약만 반환한다.
6. 목 서버 재기동 시 `pkill -f ui_mock.py`는 자기 셸까지 죽인다(-f가 명령행 문자열 매칭) — `curl`로 생존 확인 후 필요 시에만 기동.

## 렌더 회귀: 골든 스냅샷 diff

대형 렌더 전환(innerHTML→노드) 검증 표준:
1. 전환 **전** 전 탭 스윕하며 `cb.innerHTML`을 `localStorage.setItem('__golden_pre', ...)`에 저장 (36탭 ≈ 209KB, 한도 내).
2. 전환 후 SW 캐시 무효화(`serviceWorker.getRegistrations→unregister` + `caches.delete` + reload) → 재로그인 → 동일 스윕.
3. 브라우저 안에서 탭별 비교 후 요약만 반환. **style 속성은 정규화 후 비교** — 파서 경유(`style="gap:12px"`)와 cssText 경유(`style="gap: 12px;"`)는 공백/세미콜론이 다르다. 정규화: `s.replace(/\s+/g,'').replace(/;"/g,'"')` 수준.
4. 알려진 무해 diff: style 공백, 빈 class의 후행 공백. 구조(태그/순서/id/텍스트) diff는 전부 실패로 간주하고 원인 추적.

## 커버할 플로우 (키보드 트랙)

메뉴바 roving(←/→ 순환·Enter 열기·↑/↓ 순환·드롭다운 내 ←/→ 인접 메뉴·Esc), .mi Enter 활성화(3-3 위임) 후 currentTab 가로채기 없음, VM 목록 j/k + body Enter(F1), 메뉴바 Tab 1회 탈출, activity 아이콘 Enter/Space.
