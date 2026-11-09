# PureCVisor 공개 저장소 작업 규칙

저장소 범위, 코드 불변 조건, 공개 소스 정책과 검증 명령은 `AGENTS.md`를 따른다.

UI 변경은 `DESIGN.md`, `docs/adr/0013-frontend-iife-module-scope.md`와 다음 계약을 유지한다.

- Vanilla JS와 `PCV.*` 네임스페이스
- `PCV.uxlib`과 안전한 DOM 노드 빌더
- native dialog 기반 `PCV.modalCore`
- `make ui-bundle` 생성 경로
- `scripts/check_ui_bundle_fresh.py` 신선도 검사
- `npm run lint` 오류 0건

실브라우저 회귀는 `tests/ui/harness.mjs`의 Puppeteer와 `tests/ui/*.test.mjs`를 사용한다. 단계별 검증 기준은 `docs/DEVELOPMENT_VERIFICATION_POLICY.md`를 따른다.
