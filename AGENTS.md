# AGENTS.md

이 파일은 PureCVisor 공개 저장소에서 작업하는 에이전트가 따르는 기준이다.

## 저장소 범위

- Linux/KVM 기반 PureCVisor Single Edge 공개판만 다룬다.
- Multi Edge, 클러스터 자동화, 라이브 마이그레이션과 페더레이션은 공개 범위가 아니다.
- 현재 공개 경계는 `docs/PUBLIC_RELEASE_BOUNDARY.md`를 따른다.

## 문서 진입점

- 제품·설치·운영: `docs/GUIDE.md`
- 검증 정책: `docs/DEVELOPMENT_VERIFICATION_POLICY.md`
- 기능 테스트: `docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md`
- UI 시각 규격: `DESIGN.md`
- 공개 문서 사이트: `docs/PUBLIC_DOCUMENTATION_SITE.md`
- 공개 소스 정책: `docs/PUBLIC_SOURCE_POLICY.md`
- ADR 적용 상태: `docs/ADR_INDEX.md`, `docs/adr/`

## 공개 소스 규칙

- 자체 작성 소스에는 설명 주석이나 문서화 문자열을 추가하지 않는다.
- 설계 근거는 ADR과 문서에, 회귀 의도는 테스트 이름과 계약 파일에 기록한다.
- shebang과 AppArmor 전처리 지시문은 유지한다.
- `ui/vendor/`의 제3자 라이선스 고지는 변경하지 않는다.
- UI 소스맵은 생성하거나 배포하지 않는다.
- 변경 후 `make check-public-comments`를 실행한다.

## 코드 불변 규칙

- C23 `-std=gnu23`, 빌드 경고 0을 유지한다.
- 단일 프로세스와 `GMainLoop` 실행 모델을 유지하고 fork를 도입하지 않는다.
- 장시간 작업은 응답을 먼저 보낸 뒤 `GTask`로 실행한다.
- fire-and-forget 작업은 worker callback에서 실제 결과 audit과 WebSocket 완료 통지를 남긴다.
- JSON-RPC 오류 코드는 `PureRpcErrorCode`만 사용한다.
- UI 모듈은 `PCV.*` 네임스페이스와 Vanilla JS를 유지한다.
- `site/`는 제품 `ui/`와 분리된 공개 문서 build이며 Astro와 Starlight 사용을 허용한다.
- UI 작업 전 `DESIGN.md`를 확인하고 `scripts/check_design_md.py`를 실행한다.
- API 경로는 `ui/modules/endpoints.js`의 `EP` 레지스트리를 사용한다.
- DOM 문자열 삽입은 sanitizer 또는 안전한 노드 빌더를 경유한다.
- 외부 명령은 `system()`과 `popen()` 대신 `pcv_spawn_sync()` argv 배열을 사용한다.
- VM과 템플릿 이름은 핸들러 진입점에서 검증한다.

## 필수 검증

```bash
make single
make test
make check-all
make release
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
python3 scripts/check_design_md.py
python3 scripts/strip_source_comments.py --check
```

변경 범위에 맞는 추가 검증은 `docs/DEVELOPMENT_VERIFICATION_POLICY.md`를 따른다.
