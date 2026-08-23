# 공개 랜딩 아키텍처 판독성·라이트/다크 테마 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** CONDITIONAL PASS
> **대상:** `/`, `/ko/`, `/en/` Hero와 Single Edge architecture map
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046,
> `docs/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors.md`

## 1. 제품 맥락과 목표

- 사용자: 첫 화면에서 Single Edge 계층과 각 component의 기술 label을 빠르게 읽고 운영 가이드로
  이동하는 운영자와 기술 의사 결정자
- 핵심 작업: 현재 테마에서 소개 문장과 01~05 계층, component 이름·보조 기술을 확대 없이 읽고
  실제 link와 선택 경로를 식별한다.
- 변경 목표: 사용자 요청에 따라 지나치게 작은 아키텍처 글꼴과 낮은 component 대비를 개선하고,
  Starlight의 `light`·`dark`·`auto` 선택이 Hero와 architecture map 전체에 실제로 적용되게 한다.
- 비범위: 아키텍처 계층·node·정본 link·문구, 상단 navigation 구조, 운영 가이드 reader와
  Single Edge 공개 기능 범위는 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- light desktop:
  `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/before-ko-light-desktop.png`
  — `e309de461cb0f24a4ffd22e677181ef5363718e0fb2ede3648eb6164674dfc83`
- dark desktop:
  `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/before-ko-dark-desktop.png`
  — `31dddd2f6fc86fd6860ec23a5b0381d70f17ebbbbf4884b6b5fdd487265a0554`
- light mobile:
  `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/before-ko-light-mobile.png`
  — `ca9f052714ef042d21cf7218e705faef6cb7aa1cad7e8b1747f9f9970f480061`
- dark mobile:
  `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/before-ko-dark-mobile.png`
  — `6432f6eaaea4572b988e1bc8557d379e46b2f4a18d3b798d2ffe73c5a29dbe7e`
- 운영 계산 style: light와 dark 모두 Hero `rgb(5, 10, 20)`, map `rgb(9, 16, 29)`, layer
  `rgb(11, 20, 34)`, node `rgb(12, 24, 40)`로 동일하다. map title 10.88px, layer index
  9.6px, node 9.76px, node primary 10.56px, secondary 8.64px이며 mobile service
  secondary는 8.32px까지 작아진다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | 전체 글꼴과 component 가시성이 부족하고 dark/white 테마가 적용되지 않음 | 채택, 판독성·테마 분리의 직접 근거 |
| `DESIGN.md` | Visual Theme, Color Tokens, Typography, Component States | 기본은 흰 canvas·soft-gray shell·짙은 본문이며 선택형 dark surface, 판독 가능한 본문과 border+surface 상태를 요구함 | light는 white/soft surface, dark는 near-black surface, 상태는 색 외 border·surface로 구분하는 근거로 채택 |
| 기존 5계층 리뷰 | `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md` | 5계층 containment, 정본 link, load 정지, hover/focus·reduced-motion이 승인·운영 검증됨 | 정보 구조·interaction을 그대로 승계 |
| 기존 5색 리뷰 | `docs/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors.md` | sky·cyan·mint·gold·lavender를 계층 identity로 제한함 | 5색 역할은 유지하되 light에서 충분히 진한 대응색으로 조정 |
| Eraser style | [`66b9ff99-f7f2-480c-97e9-42718e49cb97`](https://refero.design/styles/66b9ff99-f7f2-480c-97e9-42718e49cb97) | technical diagram을 얇은 선·surface·mono label로 정돈함 | dense structure는 유지, 현재 8~10px 축소와 지나치게 낮은 대비는 기각 |
| Starlight 설치 source | `ThemeProvider.astro`, `ThemeSelect.astro` | 선택값을 `data-theme="light|dark"`로 적용하고 `auto`는 OS 선호를 실제 light/dark 값으로 변환함 | 별도 theme state를 만들지 않고 현재 `data-theme` 계약에 landing token을 연결 |

2026-08-24 현재 세션에는 Refero MCP 도구가 노출되지 않아 새 styles/screens 검색을 수행하지
못했다. 연결 실패를 숨기지 않고 같은 architecture map에 대해 승인된 Refero 근거, `DESIGN.md`,
설치된 Starlight theme source와 운영 화면 계산값을 대체 근거로 사용한다. 정보 구조를 바꾸지 않는
가역적인 token·type 개선이고 수용 기준이 명확하므로 CONDITIONAL PASS로 구현을 진행한다.

## 4. 결정

### 채택

- Starlight가 관리하는 `data-theme`에 landing token을 직접 연결한다. light/white는 흰 Hero,
  cool-gray map·흰 node·짙은 text를 사용하고 dark는 현재 near-black 방향을 유지하되 text와
  border 대비를 높인다. `auto`는 Starlight의 실제 light/dark 해석을 그대로 따른다.
- 5개 계층은 양 테마에서 같은 색 역할을 유지한다. light에서는 더 진한 sky·cyan·mint·gold·violet,
  dark에서는 밝은 대응색을 index·border·inset bar·connector에 사용한다.
- desktop과 mobile 모두 map title·layer index·node text·secondary label을 최소 12px로 둔다.
  service primary는 14px, Hero note는 14px 수준으로 올리고 viewport에 따른 글꼴 축소를 제거한다.
- node 최소 높이, layer padding, grid gap, service icon 크기를 함께 늘려 글자만 커지고 component가
  답답해지는 문제를 막는다. 전체 page가 길어지는 것은 허용하되 가로 overflow는 허용하지 않는다.
- normal state에서도 node가 surface와 border 양쪽으로 layer와 구분되게 한다. hover/focus는 기존
  scan·flow에 theme별 signal token을 적용하고 keyboard outline을 3px로 강화한다.

### 기각

- light에서 map만 dark island로 남기거나 Hero 배경만 바꾸지 않는다. 소개 surface 전체가 선택
  theme와 일관되어야 한다.
- 확대된 글꼴을 맞추기 위해 component 기술 label을 삭제·축약하거나 5계층 내용을 바꾸지 않는다.
- 고채도 fill, 큰 glow와 shadow로 가시성을 해결하지 않는다. white/dark surface·border·text의
  역할 대비를 먼저 사용한다.
- 별도의 landing theme toggle이나 JavaScript state를 추가하지 않는다. 기존 Starlight control과
  사용자 preference 저장 계약을 재사용한다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | theme 미적용 | UI control로 light/dark를 바꾸면 Hero·map·layer·node 계산 배경이 모두 바뀌고 reload 뒤 선택이 유지된다. |
| P0 | 작은 글꼴 | 1440·390px에서 map title, layer index, node primary·secondary 계산 글꼴이 모두 12px 이상이다. |
| P0 | component 가시성 | 양 테마 normal state에서 layer·node border와 surface가 구분되고 axe WCAG A/AA contrast 위반이 없다. |
| P1 | 반응형 | 1440·1024·768·390px 양 테마에서 page·map·node overflow와 label 겹침이 없다. |
| P1 | 상호작용 유지 | 양 테마에서 load 정지, hover/focus 선택 경로, pointer reset과 reduced-motion 계약이 유지된다. |
| P1 | 회귀 방지 | 정적 gate가 theme token, 최소 글꼴, component 크기와 기존 5계층·link·motion 계약을 함께 검사한다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 기존 Access·Service anchor의 순서를 유지하고 양 테마에서 3px solid outline과
  대응 path signal을 제공한다.
- 색상 외 표현: 01~05 index·title, layer boundary, node surface, connector와 icon을 유지한다.
- contrast: light와 dark 각각 index, primary, secondary, button, map caption을 axe로 검증한다.
- 1440/1024px: 확대된 service 4열·runtime 5열 또는 1024px 분할 grid에서 label 겹침을 확인한다.
- 768/390px: 자연 줄바꿈, 390px service 1열·나머지 compact 2열 component, button 1열과
  page 길이 증가를 확인한다.
- motion: theme 전환 자체에는 motion을 추가하지 않고 기존 link hover/focus animation만 유지한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 문제는 계산 글꼴·테마별 surface·contrast·overflow로 직접 측정할 수 있으며 heatmap이
  기술 label의 실제 판독성을 검증하지 못한다.

## 8. 구현 후 검증

- UI 자동 테스트: `/`, `/ko/`, `/en/`의 light·dark 각각 1440×1200, 1024×900,
  768×900, 390×844에서 HTTP·언어·DOM 순서·계산 style·font size·line rect·overflow를 검사했다.
  모든 map title·layer index·meta·node primary·secondary는 12px 이상이고 service primary는
  14px이다. page·map·node의 가로 overflow와 node 세로 clipping은 모두 0이다.
- theme: light 계산 배경은 Hero `rgb(255, 255, 255)`, map `rgb(238, 243, 247)`, layer
  `rgb(248, 250, 252)`, node `rgb(255, 255, 255)`이고 dark는 각각 `rgb(5, 10, 20)`,
  `rgb(9, 16, 29)`, `rgb(11, 20, 34)`, `rgb(15, 28, 45)`로 모두 다르다. UI control에서
  light·dark 선택 직후와 reload 뒤 `data-theme`, localStorage, picker 값이 일치한다.
- 접근성·상호작용: 모든 route·theme·viewport에서 axe WCAG A/AA violation과
  console·page·request 오류는 0이다. 양 테마 모두 load route animation `none`, storage
  hover·keyboard focus `pcv-arch-flow-y`, 선택 target만 강조, pointer leave 뒤 `none`이다.
  focus outline은 3px solid이고 reduced motion은 transform `none`, duration `1e-05s`, iteration
  `1`을 유지한다.
- 정적 build: `cd site && npm run check` 통과, 26 page·86 artifact와 기존 5계층·정본
  link·motion 및 새 theme token·최소 글꼴·mobile service 1열 계약을 검증했다.
- 변경 후 캡처/hash:
  - light desktop:
    `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/after-ko-light-desktop.png`
    — `61c4aca6f43ba0c4d14f0bd93a403b1b1d15b3c974b9ddafdcb85e07d5a5dce5`
  - dark desktop:
    `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/after-ko-dark-desktop.png`
    — `4006df95bb42cd9716377a7a1c3fdd2252a6e5beb6f3dd1b374fceb60a8e52d8`
  - light mobile:
    `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/after-ko-light-mobile.png`
    — `6fdc080bc70ec0872e0c4edaff9c72f7e371c2720ff8f872dd3809e8ec77e22a`
  - dark mobile:
    `.scratch/ui-reviews/2026-08-24-landing-architecture-readability-themes/after-ko-dark-mobile.png`
    — `7c4a9a98cab93db3feff1f565545589327196c88b0ceb0e2d6f28474210e4649`
- 로컬 시각 판정: desktop은 light·dark 모두 5개 layer와 node가 surface·border·text로 분리되고,
  mobile은 Capability service 1열에서 icon·이름·기술 label을 한 행으로 읽을 수 있다.
- 잔여 위험: 로컬 artifact 구현·검증까지 완료했다. 운영 반영은 별도 commit·push·Pages 배포 후
  custom domain의 실제 theme control에서 재검증해야 한다.
