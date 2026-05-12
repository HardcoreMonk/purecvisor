# purecvisor — Codex project guidance

이 repo에서 Codex가 따라야 할 프로젝트 고유 규칙.

## Purpose
PureCVisor Single Edge는 단일 노드에서 KVM VM, LXC 컨테이너, ZFS 스토리지, OVS/OVN 기반 네트워크, 백업, 인증, Web UI를 통합 관리하는 C23 기반 하이퍼바이저 오케스트레이터입니다.

## Source Of Truth
문서 간 설명이 충돌하면 아래 순서를 따른다.

1. `AGENTS.md`
2. `DESIGN.md` (UI visual contract가 있을 때)
3. `CODEX.md` (있을 때, Codex 호환용)
4. `README.md`
5. `docs/`

## Agent Workflow Contract
- 외부 agent skill pack은 참고 자료로만 사용한다. 이 repo에서 실행 가능한 규칙은
  이 파일 또는 `docs/agents/`에 있어야 한다.
- UI 작업이 있으면 `DESIGN.md`를 visual contract로 사용한다. `AGENTS.md`는 작업
  방식/보안/명령 규칙, `DESIGN.md`는 시각 방향/token/component/responsive 규칙을
  담당한다.
- Issue tracker는 GitHub를 기준으로 한다. 자세한 규칙은
  `docs/agents/issue-tracker.md`를 따른다.
- Triage labels/status: `docs/agents/triage-labels.md`가 있으면 그 매핑을 따른다.
- Domain docs: `CONTEXT.md`, `CONTEXT-MAP.md`, `docs/adr/`가 있으면 먼저 읽는다.
- Architecture pack은 조건부 표준이다. runtime/service/MCP surface가 있으면
  `docs/architecture/runtime-architecture.md`, `docs/architecture/service-logic.md`,
  필요한 경우 `docs/architecture/mcp-architecture.md`를 작성한다. 빈 placeholder는
  만들지 않는다.
- `grill-me`는 원본 installer를 설치하지 않고 Codex zone `Plan Grilling` workflow로
  사용한다. 신규 설계는 `brainstorming` 뒤 DDD `domain-architecture` pass를 거치고,
  `writing-plans` 전에 한 질문씩 검토한다.
- Lifecycle contract: `intake -> brainstorming -> domain-architecture -> grill-me ->
  plan-design-review -> writing-plans -> plan-eng-review -> implement -> code-review -> release -> operate`.
  세부 산출물 위치와 opt-out 규칙은 zone governance repo의
  `codex-project-mgmt/docs/codex-lifecycle-control-plane.md`를 따른다.
- 작업 종료 final 응답에는 `후속 작업` 목록을 포함한다. 남은 일이 없으면
  `후속 작업: 없음`으로 명시한다.
- stage, commit, push, issue publish는 사용자가 명시적으로 요청한 경우에만 한다.

## File Responsibilities
| 파일/폴더 | 역할 |
|---|---|
| `README.md` | 공개 저장소 소개, 빠른 시작, 기본 사용 예시 |
| `DESIGN.md` | Web UI visual contract |
| `docs/GUIDE.md` | 제품, 설치, 운영 통합 가이드 |
| `docs/DEVELOPMENT_VERIFICATION_POLICY.md` | 변경 유형별 검증 깊이 |
| `docs/PUBLIC_RELEASE_BOUNDARY.md` | Single Edge 공개판 경계 |
| `docs/ADR_INDEX.md` | 현재 적용되는 ADR 인덱스 |
| `docs/adr/` | 설계 결정 기록 |
| `src/` | C23 기반 daemon, API, module 구현 |
| `ui/` | Vanilla JS Web UI |
| `tests/` | 단위, 통합, 경계 검증 |
| `scripts/` | 빌드, 번들, 배포, 검증 보조 스크립트 |

## Commands
```bash
make single
make test
make check-rbac
make release
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
```

## Invariants
- 공개본 소스에는 설명 주석을 남기지 않는다.
- C 표준은 `gnu23`이며 빌드 경고 0건을 목표로 한다.
- 프론트엔드는 Vanilla JS와 `PCV.*` 네임스페이스를 유지한다.
- API endpoint는 `ui/modules/endpoints.js`의 registry를 사용한다.
- `innerHTML` 사용 시 sanitizer를 반드시 거치거나 DOM API를 사용한다.
- destructive RPC는 가능한 한 멱등성을 유지한다.
- VM/템플릿 이름은 handler 진입점에서 검증 함수 경유로 처리한다.

## Security
- secrets, 토큰, 비밀번호 commit 금지.
- 사용자 입력을 다루는 코드는 escaping, validation, authorization 규칙을 문서화한다.
