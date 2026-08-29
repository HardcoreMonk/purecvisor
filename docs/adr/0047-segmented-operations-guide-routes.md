# ADR-0047: 공개 문서는 언어·분류·문서별 정적 route를 사용한다

- **상태:** Verified
- **일자:** 2026-08-24
- **승인:** 2026-08-24 사용자 명시 승인
- **변경 승인:** 2026-08-30 일반 본문 50rem·표/코드 68rem·아키텍처 자료 75rem의
  의미 기반 reader 폭 체계를 사용자 명시 승인
- **Single Edge 적용 상태:** 공개 운영 가이드 URL·reader·navigation 계약
- **관련:** ADR-0037, ADR-0046

## Context

공개 운영 가이드는 `/docs.html` 한 shell에서 Markdown 전체를 불러오고 숫자형 hash로 22개 장을
선택했다. 이 방식은 landing에서 특정 작업을 선택해도 URL에 문서 분류가 드러나지 않고, 본문이
client fetch 뒤에 표시되며, 문서별 검색·canonical URL·이전·다음 이동을 정적으로 소유하기 어렵다.

opencodex.me와 Refero의 개발자 문서 사례는 언어·분류·문서가 포함된 고유 URL에서 좌측 전체
navigation, 중앙 본문, 우측 현재 page 목차와 이전·다음 이동을 함께 제공한다. PureCVisor는 이미
Astro·Starlight를 landing에 사용하므로 별도 reader runtime을 유지하지 않고 같은 정적 문서
기반으로 22개 장을 제공할 수 있다.

## Decision

1. `docs/GUIDE.md`는 운영 가이드 22개 장의 유일한 작성 정본으로 유지한다.
   독립 기술 문서는 각 명시된 `docs/*.md` source를 별도 작성 정본으로 사용할 수 있다.
2. `site/scripts/guide-routes.mjs`가 장 번호, 제목, 8개 그룹, directory slug, 독립 기술 문서와
   legacy hash를 단일 manifest로 소유한다.
3. build 준비 단계는 `## N. 제목` 장을 분할하고 독립 source를 변환해
   `site/src/content/docs/ko/<분류>/<문서>.md`를 결정적으로 생성한다. 첫 독립 source는
   `docs/DATABASE_STRUCTURE.md`, route는 `/ko/development/database-architecture/`다.
4. 각 독립 page의 원래 H3 이하 heading은 H2 이하로 한 단계 승격하고 code fence 안의 `#`는
   변경하지 않는다.
5. 전체 운영 가이드 기본 진입은 `/ko/getting-started/installation/`이다.
6. landing, 영어 landing과 상단 disclosure의 가이드 action은 모두 22개 새 정본 route를
   사용하며 `/docs.html#...`을 신규 navigation에 사용하지 않는다.
7. 영어 운영 본문 정본이 승인되기 전에는 한국어 본문을 `/en/...` 번역본으로 가장하지 않고
   영어 landing도 한국어 `/ko/...` 정본으로 연결한다.
8. `/docs.html`은 기존 bookmark를 위해 기본 진입과 숫자형 장 hash를 새 route로 보내는 정적
   호환 redirect로만 유지한다.
9. 공개 reader는 Starlight의 좌측 8개 그룹·23개 문서 navigation, 중앙 정적 본문, 우측 현재 page
   목차, Pagefind 검색, code copy, mobile drawer와 이전·다음 navigation을 사용한다.
10. 생성 artifact gate는 22개 가이드와 독립 기술 문서 route, canonical, active page, 전체
    sidebar link, landing link, legacy mapping, 공개 금지 표식과 내부 link 무결성을 검사한다.
11. reader의 일반 본문은 최대 50rem으로 중앙 정렬하고 표·code block은 최대 68rem,
    아키텍처 figure는 최대 75rem까지 선택적으로 확장한다. 이 값은 고정 최소 폭이 아니라
    각 자료의 최대 폭이며, 좁은 화면에서는 가용 폭을 사용하고 page-level 가로 스크롤을
    만들지 않는다.

## Consequences

- 각 장은 직접 공유·검색·색인 가능한 안정적인 URL과 HTML 본문을 가진다.
- 독립 기술 문서는 운영 가이드의 숫자형 장 계약을 바꾸지 않고 같은 reader와 검색에 참여한다.
- 장 순서나 slug 변경은 manifest, landing과 legacy mapping을 같은 변경 단위로 검증해야 한다.
- `ui/docs.html`과 `ui/guide-content.md`는 제품 Web UI 문서 shell에만 남고 공개 Pages build의
  reader dependency가 아니다.
- `/docs.html`의 기존 hash는 호환되지만 신규 URL 정본은 directory route이므로 외부 문서는
  점진적으로 새 링크로 갱신해야 한다.
- 일반 문장의 줄 길이와 기존 위치를 보존하면서 열이 많은 표, 긴 code block과 아키텍처 자료만
  더 넓은 reader canvas를 사용할 수 있다.

## Rejected alternatives

- 생성 Markdown 수작업 복제: `docs/GUIDE.md`와 독립 `docs/*.md` source에서 빠르게 어긋나는
  이중 정본이 된다.
- `/docs.html` client reader 유지: 본문 즉시 노출과 문서별 canonical URL 요구를 만족하지 못한다.
- 한국어 본문을 `/en/...`에 복제: 번역 완료 상태를 잘못 표현한다.
- 새 문서 framework 도입: 현재 Starlight가 필요한 sidebar, 목차, 검색과 pagination을 제공한다.

## Verification

- `npm run check`가 22개 가이드와 독립 기술 문서 route, 전체 sidebar와 내부 link를 검증해야 한다.
- 데이터베이스 아키텍처 HTML에는 SQLite 10개와 Audit·Monitoring Evidence·Web Push,
  일관성·장애·백업 경계가 정적으로 존재하고 private 운영 증거가 없어야 한다.
- `/ko/getting-started/installation/` HTML에 H1과 `2.1 시스템 요구사항` 본문이 정적으로 있어야 한다.
- 설치 page sidebar는 8개 그룹·22개 링크와 현재 page를 표시하고 이전은 시작하기, 다음은 VM
  관리로 연결해야 한다.
- landing과 Header 산출물에 `/docs.html` 신규 link가 없어야 한다.
- `/docs.html#3-vm-관리`는 `/ko/workloads/virtual-machines/`로 이동해야 한다.
- 1440·1280·390px 실제 browser에서 overflow, 접근성, console·page·request 오류가 없어야 한다.
- 1920px에서 일반 본문은 50rem, 표·code block은 68rem, 아키텍처 자료는 75rem의 상한을
  사용해야 하며, 1440px 이하에서는 가용 폭으로 축소되어야 한다.
- Pages 배포와 custom domain 확인 전에는 상태를 `Verified`로 올리지 않는다.

2026-08-24 구현 commit `2c7a9d8`의 GitHub Pages run
[`32652592291`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32652592291)이 성공했다.
`purecvisor.site`에서 22개 route의 HTTP 200, 제목·canonical·현재 sidebar item, 한국어·영어
landing의 운영 가이드 진입, `/docs.html#3-vm-관리` 호환 이동, desktop·mobile overflow 0,
axe와 browser 오류 0을 확인해 `Verified`로 전환했다.

2026-08-30 데이터베이스 아키텍처 독립 route와 23개 문서 sidebar 확장을 로컬 구현하고
`npm run check`에서 27개 page·90개 artifact를 검증했다. 확장 범위의 Pages·custom domain
검증 전까지 상태를 `Implemented`로 되돌린다.

구현 commit `d07e4c253e86a09583da7fee054c12f59855b919`의 GitHub Pages run
[`33277437435`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33277437435)이 성공했다.
custom domain의 데이터베이스 아키텍처 route에서 HTTP 200, canonical, 23개 sidebar link,
정적 본문, Pagefind 검색, desktop·mobile page overflow 0, Axe와 browser 오류 0을 확인해
확장 범위도 `Verified`로 전환했다.

2026-08-30 의미 기반 reader 폭 구현 commit
`a82d1f4d9eca2aec6352322a0497872f18700124`의 GitHub Pages run
[`33281197096`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33281197096)이 성공했다.
custom domain의 1920px reader에서 일반 본문 800px, 표·code block 1,088px, 바깥
아키텍처 canvas 1,200px를 확인했다. 1440px·390px에서는 가용 폭으로 축소되고 page-level
overflow, Axe와 browser 오류가 0이며 기존 릴리스 문단의 위치·세 줄도 유지되어 `Verified`로
전환했다.
