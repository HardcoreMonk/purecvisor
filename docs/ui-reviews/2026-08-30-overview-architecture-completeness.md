# 시작하기 아키텍처 개요 완전성 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LOCAL-PASS — 구현·로컬 검증 완료, GitHub Pages 확인 대기
> **승인:** 2026-08-30 사용자 명시 승인
> **대상:** `/ko/getting-started/overview/#12-아키텍처-개요`
> **관련 리뷰:** `2026-08-24-landing-service-architecture-completion-domains.md`,
> `2026-08-25-landing-service-architecture-source-svg.md`,
> `2026-08-30-public-documentation-reading-axis.md`

## 1. 제품 맥락과 목표

- 사용자: Single Edge의 요청 처리, 영속 상태와 Linux/KVM host 경계를 한 페이지에서 파악하려는
  운영자, 개발자와 기술 의사 결정자
- 핵심 작업: 접근 방식부터 transport·policy·완료 경로, 서비스 domain, DB·desired state와
  actual state까지 전체 구조를 읽고 상세 운영 문서로 이동한다.
- 변경 목표: REST/UDS 중심 ASCII 요약을 전체 아키텍처 정본과 일치시키고 모바일에서도 중요한
  내용이 가로 scroll 뒤에만 존재하지 않게 한다.
- 비범위: SVG node·connector·색상 변경, Multi Edge 기능 추가, 함수별 호출 그래프와 운영 실측
  이력 노출

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-30 KST
- 라이브 URL: `https://purecvisor.site/ko/getting-started/overview/#12-아키텍처-개요`
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-30-overview-architecture-completeness/live-desktop-detail.png`
- desktop SHA-256: `f69005370183caf59416246e569289f6c2526df549d292dc1d15d5bf44f104f4`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-30-overview-architecture-completeness/live-mobile-detail.png`
- mobile SHA-256: `788c26535cb4faa7226e0f5bb17bc28d9883e9e40ee4ebc6e1d64ce71b0edf4c`

현행 본문은 5개 coarse layer의 ASCII와 요청 단계 5개만 제공한다. 전체 SVG의 사용자·TLS·부팅
입력·transport·GMainLoop 제어면·6개 domain·영속 상태·host 책임 가운데 부팅 입력과 영속 상태는
없고 나머지도 일부만 표시한다. `auth_manager`는 실소스 모듈명이 아니며 handler 경로를
`dispatcher`와 `network`만으로 한정한 설명도 현재 구조와 맞지 않는다.

1920px에서 ASCII block은 `800px` 폭이다. 390px에서는 가용 `356px`보다 `177px` 넓은
`533px` scroll width를 요구해 오른쪽 label을 보려면 내부 가로 scroll이 필요하다. page-level
overflow와 browser 오류는 없지만, 아키텍처의 유일한 시각 설명이 이 영역에 의존해 작은 화면의
정보 접근성이 낮다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 라이브 검토 | 위 대상 URL | 아키텍처 내용이 실제 제품 범위보다 지나치게 축약됨 | 채택 — P0 완전성 문제 |
| Refero Relume sitemap | `cf15c67b-71cf-4a9b-aa37-d021f95e65d9` | 얇은 connector와 명시적 node hierarchy가 복잡한 구조 탐색을 지원 | 부분 채택 — 기존 SVG 계층 유지 |
| 선행 Refero Tailscale·PlanetScale | `5d884659-1d6b-4b82-8ccd-dbb0434667a8`, `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 차분한 infrastructure 문서와 제한된 색, 본문·diagram 역할 분리 | 채택 — 기존 PureCVisor shell 유지 |
| 전체 서비스 아키텍처 SVG | `/assets/diagrams/purecvisor-single-full-architecture.svg` | 7개 색상 layer와 32개 node에 transport, 완료, 6개 domain, DB와 host를 포함 | 채택 — 단일 시각 정본 재사용 |
| `DATABASE_STRUCTURE.md` | 로컬 작성 정본 | DB 10개, desired/actual state, 일관성·복구 경계 | 채택 — 상세 설명 link |
| ADR-0001·0012·0018·0029 | 로컬 설계 정본 | 단일 프로세스, accepted와 완료 분리, audit 결과, 두 TLS 모드 | 채택 — 요청 흐름과 경계 |

Reference lock은 기존 전체 SVG, Starlight reader, 50rem 본문과 75rem 아키텍처 figure다. 새
diagram이나 색상 체계를 만들지 않고, SVG를 확대 가능한 설명 media로 재사용하며 같은 내용을
mobile reflow 가능한 본문으로도 제공한다.

## 4. 결정

### 채택

- ASCII diagram을 제거하고 landing과 같은 SVG asset을 직접 사용한다.
- figure에 원본 확대 link, 구체적인 alt와 선택형 NGINX 경계 설명을 제공한다.
- 런타임·접근, 요청·완료, 6개 domain, DB·host, 공개 경계를 H3 단위로 분리한다.
- SQLite DB 10개를 본문에도 명시하고 독립 데이터베이스 아키텍처 page로 연결한다.
- 기존 검증 문서와 ADR quick view는 아키텍처 하위 근거로 이동하고 과거 anchor alias를 보존한다.

### 기각

- 전체 아키텍처 Markdown의 commit, node IP와 실노드 `LIVE-PASS` 이력을 overview에 복제하지 않는다.
- SVG와 별개인 두 번째 diagram을 만들지 않는다. 두 정본의 drift를 다시 만들기 때문이다.
- 모바일에서 SVG label 판독만 요구하지 않는다. 책임과 경계는 본문 목록에서도 완결한다.
- 범위 밖 Multi Edge 기능을 사용자 기능처럼 확장하지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 전체 구조 누락 | 부팅·TLS·4 transport·제어면·완료·6 domain·영속 상태·host가 본문에 있다. |
| P0 | 영속 상태 누락 | SQLite DB 10개, desired/actual state와 DB 상세 page link가 있다. |
| P0 | 비동기 결과 오해 | Job ID, worker callback, Job DB·audit·WebSocket/polling과 `accepted ≠ success`가 명시된다. |
| P0 | 오래된 구조명 | `auth_manager`, ASCII box와 dispatcher/network 한정 handler 설명이 없다. |
| P0 | 정본 분리 | overview와 landing이 같은 SVG asset을 직접 사용한다. |
| P1 | deep link 회귀 | `#12-아키텍처-개요`, 과거 `#121-검증-문서-맵`, `#122-설계-결정-빠른-보기`가 동작한다. |
| P1 | 반응형·접근성 | 1920·1280·390px에서 page overflow, 겹침, console error와 Axe A/AA 위반이 없다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 전체 SVG canvas와 확대 action은 keyboard focus와 새 탭 열기를 지원한다.
- 색상 외 상태 표현: figure의 색상은 보조 표현이고 본문 heading·label이 같은 책임을 텍스트로 전달한다.
- loading/empty/error/disabled: 정적 same-origin SVG이며 별도 상태 UI는 만들지 않는다. build gate가
  asset 부재와 hash 변조를 차단한다.
- 1024/768/480px: figure는 가용 폭에 맞춰 축소하고 본문 목록은 자연 reflow한다. SVG 세부 label은
  확대 link로 보완하며 architecture 정보 자체는 가로 scroll에 의존하지 않는다.

## 7. 정량 검증

- Attention Insight: 사용하지 않음. 문제는 시선 경쟁이 아니라 정본 대비 내용 누락과 mobile
  horizontal dependency다.

## 8. 구현 후 검증

- `cd site && npm run check`: PASS, 27개 page와 90개 artifact, overview architecture
  완전성·stale ASCII 부재 gate 통과
- `python3 scripts/check_design_md.py`, `bash tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`, `git diff --check`: PASS
- `make single`, `make test`: PASS, 1,370개 g_test와 audit startup 5개 통과
- `make check-all`: PASS, 공개 계약 38개 gate 통과
- `make release`: PASS, C23 release build 경고 0
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS

Chromium 152에서 `/ko/getting-started/overview/#12-아키텍처-개요`를 light·dark 각각
1920×1080, 1280×900, 390×844로 확인했다. 모든 조합에서 page-level overflow,
console·page·request 오류와 Axe WCAG A/AA 위반은 0이었다.

- 1920px: reader 1,200px, figure 1,200px, SVG 1,198×1,812px
- 1280px: reader·figure 624px, SVG 622×941px
- 390px: reader·figure 358px, SVG 356×539px
- SVG canvas keyboard focus와 accessible name, 원본 link, asset HTTP 200 `image/svg+xml` 확인
- H2 아래 H3 7개가 중첩되고 과거 `#121-검증-문서-맵`,
  `#122-설계-결정-빠른-보기` alias가 모두 존재
- 아키텍처 구간의 ASCII `pre`는 0개다. mobile 내부 overflow는 기존 검증·ADR 표 2개뿐이며
  새 아키텍처 설명과 figure에는 없다.

로컬 시각 증거 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| 1920px light figure | `2d40436b02cd127619c09fa359796d9d94fa34d5acc1eb87763526d90f2b1297` |
| 1920px dark figure | `69147c4bd4929e713cbf7c841b72847c77d83f717ac6027ba275052c4f83c835` |
| 390px light figure | `5b55ec6e4bed411b56c42869b327be2287193eabf41e714a3d9f7c9848da4b9d` |
| 390px dark figure | `7b587c381ca1967f3014249b3a0c98818e56c471578f0bdd6d25c0b7af860956` |
| 1920px light architecture copy | `bc45e2f9f6fee394045b99c441db7491d56fb3984ee079bd64bea78c8345ab07` |
| 390px light architecture copy | `4b9736d6da9990eeeb7fd3f0cf4991d7d2344e9f6738509865d0009c33175f0d` |

잔여 위험은 GitHub Pages와 custom domain의 artifact 반영 여부뿐이며 배포 뒤 같은 검증을
반복한다.
