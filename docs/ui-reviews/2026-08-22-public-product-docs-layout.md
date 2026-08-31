# 공개 docs 제품 문서 포털 레이아웃 리뷰

> 상태: Verified
> 대상: `https://purecvisor.site/docs.html`
> 비교 기준: 제품 노드의 `/ui/docs.html`(공개본에서는 운영 주소 비식별화)
> 구현 승인: 2026-08-22 사용자 명시 요청
>
> **2026-08-31 successor:** 현재 Pages 정보 구조와 검증은
> [공개 사이트 현재 상태 리뷰](2026-08-30-public-site-current-state-refresh.md)를 따른다.
> 이 문서의 당시 제품 노드 별칭은 공개 비노출 정책에 맞춰 비식별화했다.

## 목표와 사용자 작업

공개 사이트의 전체 운영 가이드를 Starlight 기본 reader가 아니라 제품 Web UI와 같은 통합
문서 포털로 제공한다. 사용자는 문서 홈과 장별 deep link에서 같은 header, 검색, 좌측 전체 장
navigation, 중앙 본문과 우측 현재 장 목차를 사용해야 한다.

주요 사용자 작업은 다음과 같다.

1. 공개 landing에서 22개 장 중 필요한 장으로 바로 이동한다.
2. 좌측 장 navigation과 우측 현재 장 목차로 긴 가이드를 탐색한다.
3. 검색으로 `##`와 `###` 단위 결과를 찾고 해당 절로 이동한다.
4. 모바일에서 장 drawer와 현재 절 navigation으로 같은 작업을 수행한다.

## 현재 화면 캡처

2026-08-22 01:30 KST에 데스크톱 `1440 × 1000` viewport로 현재 화면을 캡처했다. 원본은
`.scratch/ui-reviews/public-docs-product-layout/`에 두고 저장소에는 포함하지 않는다.

| 화면 | 파일 | SHA-256 |
|---|---|---|
| 제품 노드 deep link | `product-desktop.png` | `5a8fd5bcca0d8f00024a267db1f8cc5b906b81cfcd5e865a989dbb97f1087825` |
| 현재 공개 Starlight reader | `public-desktop.png` | `4637d853e5d21c9affbcd3d5234d3b338eb54fdbde00df2e8170c84bdfcc6f6b` |
| 공개 저장소 제품 shell 로컬 렌더 | `public-repo-ui-docs-desktop.png` | `89da497cf712144d62b41c2dcb850391c8c73b8e42de3dc66965b207f5c491ce` |

현재 공개 화면은 Starlight sidebar가 문서 홈과 전체 운영 가이드 두 항목만 제공하고, 우측
목차에는 22개 장이 한 목록으로 노출된다. 제품 화면은 `ui/docs.html`의 검증된 문서 shell을
사용해 좌측에서 장을 작업 category별로 묶고 중앙 본문과 우측 현재 장 목차를 분리한다.

## 근거 연구

### 저장소와 실제 제품

- `DESIGN.md`의 Documentation Portal 계약은 `ui/docs.html`을 3열 reader, 모바일 drawer,
  로컬 검색과 정확한 deep link의 정본으로 정의한다.
- 비식별 제품 노드에서 내려받은 `ui/docs.html`은 당시 검증 작업공간의 같은 파일과 SHA-256
  `67962cdef5eb0b1a014838d2ccad7e52e555305fc5d30977dd85261c1932c33e`로 일치한다.
- 공개 저장소의 `ui/docs.html`도 같은 reader 구조와 token을 가지며 공개 정책 검증을 통과한
  source다. 공개 Pages는 이 source를 최종 artifact로 게시하지 않아 시각 차이가 발생했다.

### Refero

- OpenAI Developers style `44317718-1e56-45e0-8de3-7ede70f34349`: 흰 canvas, soft gray,
  제한된 black active state, compact search와 1200px 안팎의 문서 layout을 채택 근거로 삼는다.
- Make help screen `25896602-e5bf-4bf2-8765-9cf6355af6fe`: 좌측 navigation, 640~760px 중앙
  본문과 약 220px 우측 목차의 3열 reader를 채택 근거로 삼는다.
- Raycast developers screen `a9eaeb8b-801b-4138-9829-4581e2191173`: desktop 3열과 모바일
  sidebar collapse, 검색과 deep link 흐름을 채택 근거로 삼는다.

`refero-design` 보조 스킬은 설치되어 있지 않았지만 Refero MCP 조회는 정상 완료했다. 별도
스킬 설치 없이 위 화면·스타일 ID와 저장소의 승인된 제품 UI 계약을 함께 사용했다.

## 채택 결정

- Astro·Starlight는 공개 서비스 landing과 Pages build를 계속 소유한다.
- 최종 `/docs.html` artifact는 공개 저장소의 `ui/docs.html`을 사용한다.
- reader 콘텐츠는 같은 공개 릴리스의 `ui/guide-content.md`를 사용하고 상대 source link는
  공개 GitHub 저장소 URL로 바꾼다.
- 제품의 `/ui/` base는 공개 domain root `/`로 정규화하고 제품 전용 `운영 콘솔` 진입은
  공개 `서비스 홈`으로 바꾼다.
- 공개 landing의 22개 장 링크는 제품 reader의 slug 규칙과 정확히 일치시킨다.
- 제품 shell의 Pretendard와 Coolicons는 공개 artifact에 같은 경로로 포함한다.

## 기각 결정

- 제품 노드 화면을 iframe으로 삽입하거나 public browser가 내부 주소를 fetch하지 않는다.
- private 작업공간의 `ui/docs.html`과 운영 측정값이 포함된 `guide-content.md`를 그대로
  공개하지 않는다.
- 제품 문서 shell과 유사한 별도 Starlight theme를 다시 구현하지 않는다. 같은 공개 source를
  build 단계에서 사용해 두 구현의 drift를 없앤다.
- 폐기한 `guide.html` redirect나 호환 artifact를 다시 만들지 않는다.

## 정보 구조

1. 공개 서비스 landing: 서비스 소개, 시작 흐름, 기능, 공개 범위와 문서 directory
2. `/docs.html`: 제품 문서 landing, 검색, 빠른 진입과 8개 category·22개 장
3. `/docs.html#<장>`: floating header, 좌측 전체 장 navigation, 중앙 본문, 우측 현재 장 목차
4. `1100px` 이하: 모바일 현재 절 bar와 장 drawer

## 수용 기준

- `/docs.html#1-시작하기`가 당시 제품 노드와 같은 3열 reader 구조와 active 장을 표시한다.
- 공개 landing의 22개 장 링크가 제품 reader의 실제 heading ID를 가리킨다.
- 문서 검색, 좌우 목차, 이전·다음 장과 code copy가 동작한다.
- `390 × 844`에서 좌측 navigation이 drawer로 전환되고 수평 overflow가 없다.
- 문서 로드 실패 상태에서도 정적 category와 Markdown 원문 복구 경로가 남는다.
- `/guide.html`은 404이고 artifact와 navigation link가 없다.
- Pages artifact에 source map, private repository 표식과 내부 노드 주소가 없다.
- axe 자동 검사, browser console·page error와 failed request가 0이다.

## 판정

제품 shell 자체를 재설계하지 않고 승인된 공개 `ui/docs.html`을 Pages 경로에 연결하는 변경이다.
문제, source, 공개 경계와 수용 기준이 명확하므로 구현 진입을 `PASS`로 판정한다.

## 로컬 검증 결과

2026-08-22 정적 build와 로컬 HTTP artifact를 기준으로 검증했다.

- `npm run check`: 46개 Pages artifact와 제품 reader shell, guide 콘텐츠, local font·icon,
  22개 deep link, 내부 주소·source map 부재 검증 통과
- `tests/ui/docs-portal.test.mjs`: 검색, 오류 fallback, 전체 Markdown 반영, 좌우 목차,
  1440·1280·1024·768·480px와 접근성 7개 테스트 통과
- 공개 artifact 데스크톱 캡처: `artifact-final-desktop.png`, SHA-256
  `73b416d0bd64d2e29abdfc0798679e727f32749280d3d8656e0d8717cfbee602`
- 공개 artifact 모바일 캡처: `artifact-final-mobile.png`, SHA-256
  `16a8d557ae656e1e381de9aef73ace12eca0e4b84267ddc0aaec892db7c7383c`
- 데스크톱과 모바일 수평 overflow 0, search 결과 8개 제한, 마지막 장과 active 장 확인
- axe 전체 문서 위반 0, browser console·page error·failed request 0
- `scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`와 `git diff --check` 통과
- UI 워크플로의 spec lifecycle 검사 스크립트는 공개 저장소에 포함되어 있지 않아 실행하지
  않았으며 public source·site 계약 gate로 대체했다.

## 운영 검증 결과

2026-08-22 main `43ce9ae08152d8df78e83a4f830721f08ab963ab`과 GitHub Pages run
`32504478741`을 기준으로 운영 검증을 완료했다.

- Pages build와 deploy job 성공
- `https://purecvisor.site/`, `/docs.html`, `/guide-content.md`, local font·icon 200
- `/guide.html` 404
- 운영 `docs.html`과 `guide-content.md`의 SHA-256이 로컬 검증 artifact와 각각 일치
- 공개 landing의 `전체 운영 가이드` 클릭이 `/docs.html`로 이동
- 데스크톱: 22개 장, 좌측 active 장, 우측 현재 장 목차, 마지막 장과 검색 확인
- 모바일: 현재 절 bar, 장 drawer와 마지막 장 확인
- 데스크톱·모바일 수평 overflow 0, axe 위반 0, browser console·page error·failed request 0
