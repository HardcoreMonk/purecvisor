# 데이터베이스 아키텍처 시각 흐름 UI 리뷰

> **일자:** 2026-08-31
> **판정:** LOCAL-PASS — 정적 build와 Chromium 시각·접근성 검증 완료, 배포 전
> **대상:** `/ko/development/database-architecture/`
> **관련 결정:** ADR-0001, ADR-0018, ADR-0045, ADR-0047

## 1. 설계 brief

데이터베이스 문서의 반복 표 구조는 검색 가능한 정본으로 유지하되, 독자가 요청 진입부터
관련 SQLite 저장소, GTask, Linux/KVM actual state와 완료 통지까지 먼저 한눈에 이해하도록
정보 순서를 바꾼다. raw Mermaid가 보이는 상태를 제거하고 공개 소스의 실제 책임 경계를
접근 가능한 정적 SVG로 표현한다.

비범위는 Multi Edge, 외부 DBMS, DB 복제·합의, 라이브 마이그레이션과 구현에 없는
Monitoring SQLite 저장소다.

## 2. 변경 전 문제

- Mermaid source가 SVG로 변환되지 않아 독자는 flowchart 문법을 직접 읽어야 했다.
- 390px 화면에서 Mermaid block은 가용 폭보다 넓고 대체 설명이나 시각적 계층이 없었다.
- DB별 `목적`, `위치와 초기화`, table heading이 같은 깊이로 반복돼 우측 목차가 과밀했다.
- 기존 문서의 10개 DB·33개 table 표기는 공개 소스의 9개 DB·26개 영구 table과 달랐다.
- 구현에 없는 `pcv_monitoring.db`와 availability writer가 데이터 흐름에 포함돼 있었다.
- Job DB가 worker queue를 실행하고 모든 GTask가 registry 행을 만드는 것처럼 읽혔다.

## 3. 연구 근거와 reference lock

| 근거 | 유지하거나 차용한 패턴 | 적용 |
|---|---|---|
| 기존 PureCVisor Starlight reader | Pretendard, 흰 canvas, 얇은 선, 50rem prose와 75rem architecture rail | site shell과 읽기 축 유지 |
| HashiCorp 기술 아키텍처 | 계층·책임·경계 이름을 먼저 드러내는 구조 | 요청, daemon, persistence, actual state를 분리 |
| IBM Carbon data pattern | 데이터 종류와 상태 의미를 색상 외 label로 중복 전달 | 실선·점선 범례와 그룹 제목 사용 |
| Tailscale·Timescale 기술 문서 | 장식보다 기능적 connector와 확대 가능한 원본 | code-native standalone SVG와 원본 link 사용 |

Reference lock은 white/neutral canvas, `#171c24` ink, teal flow, amber persistent boundary다.
gradient, glow, 장식용 3D, DB마다 다른 색, card wall과 raw Mermaid 노출은 사용하지 않는다.

## 4. 사실 관계 정리

공개 소스의 DDL과 초기화 경로를 기준으로 SQLite 파일 9개와 영구 table 26개를 확인했다.
정본과 정책 4개·19 table, 작업 상태 3개·3 table, 증거와 외부 통합 2개·4 table로 주 책임을
묶었다. Security DB처럼 정책과 증거를 함께 맡는 저장소는 복수 역할을 가질 수 있다.

`pcv_jobs.db`는 작업 실행 queue가 아니라 선택형 상태 registry다. handler가 긴 작업의
accepted 응답을 먼저 끝낸 뒤 제한된 GTask를 직접 시작하며, registry를 사용하는 경로만
행을 만든다. ZFS는 선택형이고 OVN backend는 후보 경계다. 서로 다른 DB 사이의 원자적
transaction은 없다.

## 5. 결정 ledger

| 결정 | 이유 |
|---|---|
| 요약 수치 띠를 SVG보다 먼저 배치 | 9·26·0·0의 구조적 전제를 빠르게 고정 |
| 1440×1040 standalone SVG | 외부 script·font·`foreignObject` 없이 GitHub와 Pages에서 동일 렌더링 |
| 짧은 응답과 긴 GTask를 분기 | 모든 요청이 worker를 거친다는 오해 제거 |
| handler→GTask를 `긴 작업만`, direct branch를 `accepted 먼저`로 표기 | 응답 우선 실행 규칙을 시각화 |
| 저장소를 주 책임 기준 세 묶음으로 표현 | 9개 파일을 한 줄 나열하지 않고 차이를 먼저 전달 |
| 일반 경계 `#7f8b99`, DB 경계 `#b87543` | 축소 상태에서도 기능 경계를 3:1 수준으로 구분 |
| SVG 최소 판독 폭 84rem과 내부 scroll | 14px 주요 label을 약 13px로 유지하고 page overflow 차단 |
| `### 스키마` 아래 table·index를 H4로 이동 | table 검색성은 유지하고 기본 우측 목차 과밀 완화 |
| 원본 link와 scroll 안내를 caption에 제공 | scrollbar를 숨기는 모바일 환경에서도 탐색 방법 노출 |

## 6. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | raw Mermaid 없이 실제 SVG가 표시되고 원본 link가 동작한다. |
| P0 | SVG와 본문이 9개 DB·26개 table, 외부 DBMS 0, cross-DB transaction 0을 일치시킨다. |
| P0 | 짧은 응답, 긴 작업 accepted, GTask 실행과 완료 통지가 서로 다른 경로로 읽힌다. |
| P0 | `pcv_monitoring.db`, availability writer와 실행 queue 오해가 없다. |
| P1 | SVG에 title·desc·role·viewBox가 있고 외부 resource, script와 event handler가 없다. |
| P1 | 1440px·390px에서 page overflow가 0이고 모바일 canvas가 keyboard와 touch scroll을 지원한다. |
| P1 | light·dark theme에서 Axe WCAG A/AA 위반과 console·page·request 실패가 0이다. |
| P1 | table·index heading은 H4이며 9개 `스키마`만 H3 목차에 노출된다. |

## 7. 구현

- `DATABASE_STRUCTURE.md`를 9개 DB·26개 table 기준으로 정리하고 SVG figure를 registry보다
  앞에 배치했다.
- SVG는 요청·응답, 관련 policy·registry·audit, reconcile, Linux/KVM actual state와 Web Push
  외부 전달을 하나의 흐름으로 구성했다.
- summary는 연결된 4분할 띠로 만들고 4→2→1열로 reflow한다.
- 좁은 reader에서도 SVG를 과도하게 축소하지 않고 figure 내부만 가로 scroll하도록 했다.
- asset URL rewrite, SVG checksum·안전성·구조, heading hierarchy와 반응형 CSS를 artifact gate에
  추가했다.
- GUIDE의 NGINX 유무별 전체 아키텍처 SVG도 같은 9개 DB 사실 관계로 동기화하고 재현 가능한
  Mermaid source를 함께 관리한다.

## 8. 로컬 검증

`cd site && npm run check`, XML parse와 `git diff --check`가 통과했다. Chromium 152에서
데이터베이스 page를 1440×1000 light·dark와 390×844 light로 확인했다.

저장소 필수 검증인 `make single`, `make test`(g_test 1,370개와 audit startup 5개),
`make check-all`(38개 계약 gate), `make release`, `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`,
`python3 scripts/check_ui_bundle_fresh.py`, `python3 scripts/check_design_md.py`,
`python3 scripts/strip_source_comments.py --check`와 `make check-public-comments`도 모두 통과했다.

- page-level overflow: desktop 0px, mobile 0px
- summary column: desktop 4, mobile 2
- SVG natural size: 1440×1040
- mobile canvas: client 341px, scroll 1344px, 방향키 scroll `0 → 40px`
- raw Mermaid와 숫자 heading 뒤 자동 `<br>`: 없음
- Axe WCAG A/AA: light·dark·mobile 모두 0
- console·page·request 실패: 0

GUIDE overview도 1440×1000과 390×844에서 확인했다. 직접 HTTPS SVG는 초기 상태에서,
NGINX SVG는 탭 선택 뒤 progressive inline 상태가 `ready`가 됐다. 두 asset의 inline SVG 수는
탭 선택 후 2개이며 Monitoring node rollover는 활성 node 1개와 직접 연결 edge 2개를 강조했다.
두 viewport의 page overflow와 Axe 위반은 0이었다.

로컬 시각 증거 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| database SVG 원본 렌더 | `e5646db2e7ba9a42ec61bdb62f3766c4ff4fb4fcfe37a08958fcafe3c643fd3c` |
| database page 1440px dark | `3d9b3b6c1249c5667a6a0faa3bc071b664372792137614b06b03c79eae4cf0bc` |
| overview NGINX tab 1440px | `f722b1ad0fc95e2861f183ac95184f054a41f077801ba9f1097098491df9cc44` |

배포와 live 검증은 이번 작업 범위에 포함하지 않았다.
