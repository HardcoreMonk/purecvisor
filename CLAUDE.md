@../CLAUDE.md

# purecvisor-single 작업 규약 (L4)

Linux/KVM 단일 노드 하이퍼바이저. 코드 표준·문서 진입점은 [`AGENTS.md`](AGENTS.md)와 `docs/DEVELOPER_INDEX.md`를 공유한다 (숫자·현황·이력의 진실은 docs/와 git).

## UI invariants

1. **DOM-safe (zone ADR-013)**: 신규·수정 UI 코드는 `innerHTML` / `outerHTML` / `insertAdjacentHTML` / `document.write` / HTML 생성 템플릿 리터럴 금지. DOM 조립은 `PCV.uxlib.el` / `frag` / `clearEl` + `textContent`만. 레거시 innerHTML 사이트는 접점 수정 시 점진 전환 — 현황 집계는 `npm run lint:domsafe`(보고 전용, 래칫 숫자 감소만 허용).
2. **표준 상위 경로**: 상태/오류/빈상태 원라이너는 `PCV.uxlib.msg/setMsg`(4차 배치 도입), 카드·행·배지·섹션·스탯타일 컴포넌트는 노드 빌더 `HN`(`window.HN`/`PCV.ui.HN`, 5차 배치 도입). 직접 el() 조립보다 이 둘을 우선한다.
3. `ui/modules/ui.js`의 문자열 헬퍼 `H`(card/row/…)와 `escapeHtml`/`escapeAttr`는 **레거시 전용** — 신규 코드에서 사용 금지 (raw interpolation 풋건). innerHTML→노드 전환 시 문자열 보간의 `escapeHtml(x)`는 원문 `x`로 (이중 이스케이프), 속성 문자열 내 `escapeAttr`는 유지.
4. **모듈 형식**: 각 `ui/modules/*.js` = 자체 완결 `(function(PCV){...})(window.PCV)` IIFE + `PCV.*`/`window.*` 등록, bare 참조 호출 (in-repo [ADR-0013](docs/adr/0013-frontend-iife-module-scope.md)). 파일 단위로 eslint 파싱 가능해야 한다. 모듈 로드 순서상 `ui.js`가 `uxlib.js`보다 먼저 — ui.js에서 `PCV.uxlib.*`는 호출 시점 해소만 (톱레벨 별칭 금지).
5. **번들 파이프라인**: `make ui-bundle`이 정본 — 모듈 편집 후 필수 실행 (UI_MODULES 등록 가드 + esbuild 민파이 + sw.js CACHE_NAME bump). 신선도 게이트: `scripts/check_ui_bundle_fresh.py`.
6. **lint**: `npm run lint` 0 error 유지 (no-unused-vars warning은 기준선 추적).

## 검증

UI 변경은 커밋 전 실브라우저 검증 — 절차는 `.claude/skills/verify/SKILL.md` (목 API + Playwright, 데몬·자격증명 불요).
