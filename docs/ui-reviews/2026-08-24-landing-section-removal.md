# 공개 랜딩 세 구역 제거 제품 UI 리뷰

> **2026-08-24 후속:** Hero를 변경하지 않는 비범위 결정은
> `docs/ui-reviews/2026-08-24-landing-hero-copy.md`의 사용자 지정 문구 축소 결정이 대체한다.

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩 정보 구조
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046

## 1. 제품 맥락과 목표

- 사용자: PureCVisor를 처음 확인하거나 운영 문서의 특정 작업을 찾는 Single Edge 운영자
- 핵심 작업: 제품 정체성과 기본 진입을 확인한 뒤 8개 문서 카테고리·22개 장에서 필요한
  운영 절차로 바로 이동한다.
- 변경 목표: 사용자 요청에 따라 서비스 기능 소개, 설치·실행·상태 확인 시작 흐름,
  Single Edge 공개 범위 구역을 첫 페이지에서 제거하고 hero 다음에 문서 directory가 바로
  이어지게 한다.
- 비범위: hero 문구·제어면 구조, 문서 8개 카테고리·22개 장, 역할별 경로, 상단 4개
  disclosure menu와 운영 가이드 본문은 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로: `.scratch/ui-reviews/2026-08-24-landing-section-removal/before-ko-landing.png`
- SHA-256: `ed1983cf4b186df02c36db9835305cf6ff6a2ae7a5826fdf0463927e542a7f01`
- 현재 동작·데이터 계약: hero 뒤의 4개 page anchor가 서비스 기능, 시작 흐름, 공개 범위와
  문서 directory로 이동한다. 제거 대상 세 구역은 한국어와 영어 landing에 같은 구조로 있고,
  Header의 일부 하위 link도 이 anchor를 가리킨다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | `한 노드의 가상화 운영을 하나의 흐름으로.`, `설치하고. 실행하고. 상태를 확인합니다.`, `Single Edge에 집중한 공개판` 항목 삭제 | 채택, 정보 구조 변경의 직접 근거 |
| 기존 공개 landing 리뷰 | `docs/ui-reviews/2026-08-22-public-service-landing.md` | hero, 서비스 소개, 시작 흐름, 공개 범위, 문서 directory 순서를 승인함 | 시각 token·hero·문서 directory는 유지하고 세 중간 구역 순서만 이번 결정으로 승계 |
| Refero Fernand Docs | [`54c4c424-f096-4361-b07c-cdff6263aa12`](https://refero.design/pages/54c4c424-f096-4361-b07c-cdff6263aa12) | 짧은 제목·검색 다음에 문서 카테고리 grid를 배치해 탐색을 우선함 | hero 다음 문서 directory 직결 근거로 채택 |
| Refero HTTPie Docs | [`a01522c6-b9c9-472c-b753-ced6e8d945e2`](https://refero.design/pages/a01522c6-b9c9-472c-b753-ced6e8d945e2) | 한 화면의 문서 hero에서 소수 정본 문서 진입으로 바로 연결함 | 중간 마케팅 설명 없이 문서 진입을 앞당기는 근거로 부분 채택 |
| Refero Bezi Docs | [`a96b205c-6da2-487c-93c5-84318884c3b9`](https://refero.design/pages/a96b205c-6da2-487c-93c5-84318884c3b9) | 제목과 action 아래에 그룹형 문서 link를 직접 제공함 | 기존 8개 그룹 directory 유지 근거로 채택 |

`refero-design` 보조 스킬은 설치되어 있지 않으며, 이전 작업에서 안내한 설치를 새로 수행하지
않고 Refero MCP의 screen 검색·상세 조회를 직접 사용했다.

## 4. 결정

### 채택

- 한국어와 영어 landing에서 `capabilities`, `quickstart`, `scope` section 전체를 제거한다.
- 제거된 section만 가리키던 hero 하단 4칸 anchor rail도 제거한다. 문서 action은 hero의
  `전체 운영 가이드`와 이어지는 문서 directory에 이미 존재하므로 한 칸짜리 rail을 남기지 않는다.
- 상단 `서비스`, `시작하기`, `공개 범위`, `문서` disclosure label은 기존 탐색 계약이므로
  유지한다. 제거된 home anchor를 가리키던 세 link만 대응 운영 가이드 section으로 바꾼다.
- 관련 landing 전용 CSS와 build gate를 함께 제거·반전해 삭제된 section이 재도입되면 검사가
  실패하도록 한다.

### 기각

- 제목만 숨기고 카드·terminal·범위 목록을 남기지 않는다. 사용자 요청의 `항목 삭제`를
  부분적인 시각 숨김으로 해석하지 않는다.
- 삭제 공간을 새 마케팅 문구, illustration 또는 대체 card로 채우지 않는다.
- 상단 disclosure menu 자체를 제거하지 않는다. 이전 사용자 승인과 직접 문서 경로 탐색 계약을
  유지해야 한다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 세 구역과 영문 대응 구역 제거 | root·`/ko/`·`/en/` HTML에 세 section id, 제목, 카드·terminal·scope list가 없다. |
| P0 | 제거된 anchor 참조 제거 | landing과 Header에 `#capabilities`, `#quickstart`, `#scope`가 없고 하위 메뉴는 유효한 운영 가이드 URL을 사용한다. |
| P1 | 문서 중심 흐름 유지 | hero 다음 주요 section이 `documentation`이고 8개 카테고리·22개 장과 역할별 경로가 유지된다. |
| P1 | 반응형·접근성 | 1440px·390px에서 overflow가 없고 axe 및 browser 오류가 없다. |
| P2 | 불용 코드 제거 | 삭제된 section 전용 CSS selector와 긍정 build assertion이 남지 않는다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 제거되는 terminal focus region과 page anchor가 DOM에 남지 않는다. Header
  disclosure keyboard 계약은 그대로 유지한다.
- 색상 외 상태 표현: 기존 문서 카드의 텍스트·번호·link를 유지한다.
- loading/empty/error/disabled: 정적 landing이므로 해당 상태는 없고 build·내부 link gate가
  누락을 차단한다.
- 1024/768/480px: 중간 section 삭제 후 hero와 문서 directory 사이 여백이 과도하지 않은지,
  mobile 문서 card가 기존 2열·1열 계약을 유지하는지 확인한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자가 제거 범위를 명시했고, 새 시선 계층 후보 비교가 아니라 정보량 축소 작업이다.

## 8. 구현 후 검증

- UI 자동 테스트: `npm run check`가 26개 HTML과 총 86개 Pages artifact, 내부 link,
  8개 category·22개 chapter, 제거 section·anchor·CSS selector 부재를 통과했다.
- 저장소 게이트: `python3 scripts/check_design_md.py`,
  `bash tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `python3 scripts/strip_source_comments.py --check`, `git diff --check`가 통과했다.
- Chromium `1440 × 1000`: `/`, `/ko/`, `/en/`의 직접 section 순서가
  `_top → documentation → pcv-final-cta`이고 제거 section·stale anchor 0,
  8개 category·22개 chapter, 상단 4개 disclosure를 확인했다.
- Header의 `서비스 개요`, `서비스 시작 흐름`, `공개 범위 개요`와 영어 대응 link가 각각
  `/ko/getting-started/overview/`, `#14-5분-퀵스타트`, `#공개-범위-핵심` 정적 문서 경로를
  사용하는 것을 확인했다.
- keyboard `ArrowDown`으로 첫 하위 link에 진입하고 `Escape`로 trigger focus가 복원됨을
  확인했다.
- Chromium `390 × 844`: Hero 다음 문서 directory, 단일열 category·role path와 overflow 0을
  확인했다.
- root·한국어·영어·mobile의 axe 위반 0, browser console·page·request 오류 0을 확인했다.
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-section-removal/after-ko-desktop.png`
  — `d05d6f0975a2f204cc522367bd62a511afeb340405b202f62942d6e1baee3f67`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-section-removal/after-ko-mobile.png`
  — `e0e05d8501494a9ab5e5586f5547cb5dacfb25aa88e0ecee67d761c54a49f833`
- 구현 commit `39f94c5`의 GitHub Pages run
  [`32653904395`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32653904395)이 성공했다.
- `purecvisor.site`의 `/`, `/ko/`, `/en/`과 390px mobile에서 section 순서,
  제거 section·stale anchor 0, 8개 category·22개 chapter, 새 Header guide link를 재확인했다.
- 운영 domain의 axe 위반과 browser console·page·request 오류는 0이며 overflow도 0이다.
- 잔여 위험: 없음. 이후 landing section 재도입과 stale anchor는 `npm run check`가 차단한다.
