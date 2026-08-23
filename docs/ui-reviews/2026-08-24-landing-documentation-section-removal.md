# 공개 랜딩 문서 디렉터리 섹션 제거 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩의 `documentation` 섹션
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046,
> `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md`

## 1. 제품 맥락과 목표

- 사용자: 첫 화면에서 Single Edge의 범위와 계층을 확인하고 Hero·상단 메뉴·아키텍처 node에서
  필요한 운영 가이드로 이동하는 운영자와 기술 의사 결정자
- 핵심 작업: Hero의 두 action 또는 실제 아키텍처 service link로 빠르게 운영 가이드에 진입한다.
- 변경 목표: 사용자 요청에 따라 `필요한 작업에서 시작하세요.`가 포함된 문서 디렉터리 섹션
  전체를 제거하고 Single Edge Hero를 첫 페이지의 유일한 본문으로 만든다.
- 비범위: Hero copy·action·5계층 지도, 상단 4개 disclosure navigation, 22개 운영 가이드 page,
  `/docs.html` 호환 redirect와 Starlight reader는 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/live-ko-desktop.png`
  — `03be7edc49b5f0b491034fb3f9d7178c0553eaeebd9e840e65fe9c233d0e6867`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-mobile.png`
  — `a6b1e9e861c0aa4470da73e3a1eadb543aee4193214da0c626204fcb98468cf0`
- 현재 동작·데이터 계약: 5계층 Hero 뒤에 빠른 진입 4개, 8개 카테고리·22개 장, 역할별
  추천 경로 3개가 하나의 `documentation` 섹션으로 이어진다. 운영 가이드는 이 섹션과 별개로
  Hero, 상단 disclosure와 아키텍처 Access·Service link에서도 진입할 수 있다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | `필요한 작업에서 시작하세요.` 섹터 삭제 | 채택, `documentation` 전체 삭제의 직접 근거 |
| Single Edge 아키텍처 리뷰 | `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md` | Hero 안에서 Access 3개·Service 4개가 실제 운영 가이드 link이고 5계층 구조가 독립적인 소개 자료 역할을 함 | Hero를 유일한 본문으로 유지할 수 있는 탐색·콘텐츠 근거로 채택 |
| 기존 랜딩 축소 리뷰 | `docs/ui-reviews/2026-08-24-landing-section-removal.md` | 반복 마케팅 구역을 제거하고 정본 문서 진입을 남기는 원칙을 승인함 | 대체 마케팅 구역을 추가하지 않는 근거로 채택, 문서 directory 유지 결정은 이번 요청이 대체 |
| 최종 CTA 제거 리뷰 | `docs/ui-reviews/2026-08-24-landing-final-cta-removal.md` | Hero action과 직접 문서 link가 있으면 반복 하단 CTA를 제거함 | 삭제 뒤 새 CTA를 만들지 않는 근거로 채택 |
| Refero Fernand Docs | [`54c4c424-f096-4361-b07c-cdff6263aa12`](https://refero.design/pages/54c4c424-f096-4361-b07c-cdff6263aa12) | 문서 category 중심 랜딩의 기존 근거 | 이번에는 사용자가 category 섹션 자체를 제거하므로 구조 채택은 철회, 정본 문서 page 유지 원칙만 승계 |
| Refero HTTPie Docs | [`a01522c6-b9c9-472c-b753-ced6e8d945e2`](https://refero.design/pages/a01522c6-b9c9-472c-b753-ced6e8d945e2) | 소수 정본 action으로 문서 진입을 압축함 | Hero의 두 action과 상단 메뉴를 유지하는 근거로 부분 채택 |

`refero-design` 보조 스킬이 설치되어 있지 않아 새 외부 검색을 수행하지 않고, 같은 landing에
대해 승인·운영 검증된 Refero 조사와 로컬 리뷰를 재사용했다. 사용자가 삭제 범위를 직접
지정했고 되돌리기 어려운 새 시각 선택이 없으므로 작업을 중단하지 않는다.

## 4. 결정

### 채택

- 한국어 `필요한 작업에서 시작하세요.`와 영어 `Start with the task you need.`가 포함된
  `documentation` section 전체를 root·`/ko/`·`/en/`에서 제거한다.
- 빠른 진입 4개, 문서 category 8개·chapter link 22개와 역할별 추천 경로 3개를 함께 제거한다.
- Hero의 5분 퀵스타트·전체 운영 가이드 action, 상단 disclosure의 운영 가이드 link,
  아키텍처 Access 3개·Service 4개 link를 유지한다.
- 22개 운영 가이드 route, 8개 Starlight sidebar group, 검색·이전/다음·legacy redirect는
  공개 문서 reader 계약이므로 계속 생성·검증한다.
- 제거된 전용 CSS와 landing chapter-link 긍정 assertion을 삭제하고, section·문구·class가
  재도입되면 실패하는 반사실 정적 gate로 바꾼다.

### 기각

- 제목만 숨기고 category card나 역할별 경로를 남기지 않는다. `섹터 삭제`를 전체 section
  삭제로 적용한다.
- 삭제 공간을 새 slogan, CTA, illustration 또는 footer 전용 여백으로 채우지 않는다.
- 운영 가이드 page나 Header의 `문서` 메뉴까지 제거하지 않는다. 첫 페이지 정보량 축소와
  정본 문서 접근성은 서로 다른 계약이다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 문서 섹션 전체 제거 | 세 landing의 직접 section id가 `_top` 하나이고 `documentation`, KO·EN 제목과 전용 class가 없다. |
| P0 | 정본 문서 접근 유지 | Hero action 2개, 상단 disclosure 4개, 아키텍처 Access 3개·Service 4개 link와 22개 가이드 page가 유효하다. |
| P1 | 불용 코드 제거 | 문서 directory·category·role path 전용 CSS와 landing 22개 link 긍정 assertion이 없다. |
| P1 | Hero→footer 연결 | 1440px·390px에서 Hero 뒤 공통 footer가 불필요한 빈 section 없이 자연스럽게 이어진다. |
| P1 | 반응형·접근성 | 세 route의 desktop·mobile에서 horizontal overflow, axe·browser 오류가 없다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 제거되는 문서 link 29개가 tab 순서에서 사라진다. Hero·상단 메뉴·아키텍처
  link의 focus contract는 유지한다.
- 색상 외 상태 표현: 5계층 지도는 text·index·border·connector를 유지해 정지 상태에서도 읽힌다.
- loading/empty/error/disabled: 정적 Hero와 link이므로 별도 상태가 없다.
- 1024/768/480px: Hero의 기존 2열→1열 계약과 footer 시작 위치, page·map overflow를 확인한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자가 제거 대상을 명시한 정보 구조 축소이며 새 레이아웃 후보의 시선 비교가 아니다.

## 8. 구현 후 검증

- UI 자동 테스트:
  - Puppeteer에서 `/`, `/ko/`, `/en/` 각각 1440×1000·390×844 검증
  - 직접 section id는 `_top` 하나, `documentation` DOM·KO/EN 제목·`RECOMMENDED PATHS` 0건
  - Hero action 2개, 상단 navigation group 4개, Access link 3개·Service link 4개·Layer 5개 유지
  - page·map horizontal overflow, console·page·request error와 axe WCAG A/AA violation 0
  - Hero와 공통 footer 사이 계산 간격 24px로 불필요한 빈 section 없이 연결
- interaction 회귀:
  - load connector animation `none`, storage hover `pcv-arch-flow-y`, 대응 target만 강조
  - pointer 이탈 뒤 `none`, keyboard focus outline `solid`와 hover 동일 flow
  - reduced motion에서 transform `none`, animation `0.01ms`·1회
- 정적 build: `cd site && npm run check` 통과, 26 page·86 artifact와 내부 link, 22개 가이드
  route·8개 sidebar group·legacy redirect를 유지하고 제거 section·문구·CSS 재도입을 차단
- 변경 후 캡처/hash:
  - `.scratch/ui-reviews/2026-08-24-landing-documentation-section-removal/after-ko-desktop.png`
    — `9c3f8f745881313488c9d47faa96956fbadeccb72ba85908f936587563e064aa`
  - `.scratch/ui-reviews/2026-08-24-landing-documentation-section-removal/after-ko-mobile.png`
    — `aa461e170c584e05ab792f6aa23eabf28666965f4cb77fe82e92f28346330f70`
- 시각 판정: desktop은 Hero copy와 5계층 지도가 한 화면의 유일한 핵심으로 남고, mobile은
  지도 뒤 footer가 바로 이어져 제거된 문서 목록의 빈 공간이나 중복 CTA가 없다.
- 구현 commit `1ae6bb4`의 GitHub Pages run
  [`32663611014`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32663611014)이 build와 deploy
  두 job을 모두 성공했다.
- `purecvisor.site`의 `/`, `/ko/`, `/en/`을 1440×1000·390×844에서 다시 확인한 결과 모든
  경로가 HTTP 200, 직접 section `_top` 하나, 제거 DOM·KO/EN 문구 0건을 유지했다. Hero action
  2개, 상단 navigation group 4개, Access 3개·Service 4개·Layer 5개도 그대로다.
- 운영 domain의 page·map overflow, console·page·request error와 axe WCAG A/AA violation은
  0이다. hover·pointer 이탈·keyboard focus·reduced-motion 계약도 로컬 결과와 일치했다.
- 운영 desktop 캡처 SHA-256
  `9c3f8f745881313488c9d47faa96956fbadeccb72ba85908f936587563e064aa`와 mobile 캡처
  `aa461e170c584e05ab792f6aa23eabf28666965f4cb77fe82e92f28346330f70`은 로컬 검증값과
  각각 일치했다.
- 잔여 위험: 없음. 제거 section·문구·전용 CSS 재도입은 `npm run check`가 차단한다.
