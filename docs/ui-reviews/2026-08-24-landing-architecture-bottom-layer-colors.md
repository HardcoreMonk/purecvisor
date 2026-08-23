# 공개 랜딩 아키텍처 하단 배치·5색 계층 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero copy와 Single Edge 아키텍처 지도
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046,
> `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md`

## 1. 제품 맥락과 목표

- 사용자: 첫 화면에서 Single Edge의 설명과 실제 계층을 순서대로 읽고 운영 가이드로 이동하는
  운영자와 기술 의사 결정자
- 핵심 작업: 제품 범위 문장을 한 번에 읽고, 바로 아래의 5개 계층을 색과 index로 구분한 뒤
  Access·Capability service의 정본 link를 사용한다.
- 변경 목표: 사용자 요청에 따라 우측 아키텍처 지도를 copy 아래 전체 폭으로 이동하고,
  01~05 계층에 서로 다른 식별색을 적용하며 한국어 범위 문장을 desktop 한 줄로 표시한다.
- 비범위: Hero 문구 내용·action, 아키텍처 node·link·motion, Header, 운영 가이드 reader와
  Single Edge 기능 범위는 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-documentation-section-removal/live-ko-desktop.png`
  — `9c3f8f745881313488c9d47faa96956fbadeccb72ba85908f936587563e064aa`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-documentation-section-removal/live-ko-mobile.png`
  — `aa461e170c584e05ab792f6aa23eabf28666965f4cb77fe82e92f28346330f70`
- 현재 계약: Hero는 copy와 약 31rem 너비의 map을 좌우 2열로 배치한다. 범위 문장은
  `max-width: 36rem` 때문에 desktop에서도 두 줄이고, 5개 layer의 surface 차이는 있지만 index와
  border가 같은 cyan 계열이라 한눈에 계층 경계를 찾기 어렵다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | map 하단 이동, 01~05 서로 다른 색상, 한국어 범위 문장 줄바꿈 제거 | 채택, 배치·색·desktop copy의 직접 근거 |
| 사용자 지정 Claude 공유 | [`5df29e48-c520-4dc7-9862-51475b63457d`](https://claude.ai/share/5df29e48-c520-4dc7-9862-51475b63457d) | 세로 layer마다 색을 달리해 긴 아키텍처에서 현재 위치를 빠르게 식별함 | 서로 다른 layer identity만 채택, rainbow fill·7색 범례·외부 SVG는 기각 |
| 기존 아키텍처 리뷰 | `docs/ui-reviews/2026-08-24-landing-single-edge-architecture-layers.md` | 세로 containment, 정지 정보, 실제 link, hover/focus·reduced-motion을 승인·운영 검증함 | node·link·motion 계약을 그대로 승계, 단색 layer 결정은 이번 사용자 요청이 대체 |
| Eraser style | [`66b9ff99-f7f2-480c-97e9-42718e49cb97`](https://refero.design/styles/66b9ff99-f7f2-480c-97e9-42718e49cb97) | dark diagram에서 얇은 선과 제한된 강조로 고밀도 기술 정보를 정돈함 | 채도 낮은 border·index·얇은 inset bar 적용 근거로 채택 |
| Vercel Infrastructure screen | [`d994631b-6d7a-4e07-8aa0-07788706b57d`](https://refero.design/pages/d994631b-6d7a-4e07-8aa0-07788706b57d) | dark infrastructure surface에서 accent를 정보 구분에 제한함 | 큰 색면 대신 layer identity에만 색을 쓰는 근거로 채택 |

`refero-design` 보조 스킬이 설치되어 있지 않아 새 외부 검색은 수행하지 않고, 같은 map에 대해
승인·운영 검증된 Refero·사용자 지정 자료와 로컬 리뷰를 재사용했다.

## 4. 결정

### 채택

- `.pcv-hero-grid`를 한 열로 바꾸어 copy 다음에 map을 전체 shell 폭으로 배치한다.
- 1280px급 desktop에서는 Capability services 4개와 Runtime adapters 5개를 각각 한 행으로
  배치한다. 1024px 이하는 기존 2열·분할 grid, 512px 이하는 기존 compact 2열로 전환한다.
- layer identity palette는 다음처럼 고정한다. 색은 status가 아니라 순서 식별 전용이다.
  - 01 Access: sky `#78a9e6`
  - 02 Control plane: cyan `#63c2d4`
  - 03 Capability services: mint `#78c7a2`
  - 04 Runtime adapters: gold `#d4b06a`
  - 05 Linux host: lavender `#b79ade`
- 각 색은 layer border, 2px inset bar, index, node의 낮은 대비 border에만 사용한다. 본문·상태
  의미와 hover/focus signal은 기존 neutral·teal 역할을 유지한다.
- 한국어·영어 범위 문장은 1024px 이상에서 `white-space: nowrap`과 제한 없는 width로 한 줄을
  유지한다. 768px 이하에서는 읽을 수 없는 축소나 horizontal scroll 대신 자연 줄바꿈한다.

### 기각

- layer 전체를 고채도 rainbow 배경으로 채우거나 glow를 확대하지 않는다. 어두운 technical
  canvas와 정지 정보 판독성이 우선이다.
- 색만으로 layer를 구분하지 않는다. 01~05 index, layer title, border와 포함 관계를 유지한다.
- mobile에서 한 줄을 강제해 6px대 글자나 horizontal overflow를 만들지 않는다.
- map 내용을 가로 5단계 flow로 바꾸지 않는다. 사용자가 승인한 top-down containment를 유지한다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 하단 전체 폭 배치 | 1440px에서 copy가 map 위에 있고 두 요소의 x 시작점과 폭이 같은 shell 흐름에 속한다. |
| P0 | 5색 계층 식별 | 01~05의 계산된 index·border 색이 모두 다르고 text·index 없이 색만으로 의미를 전달하지 않는다. |
| P0 | 범위 문장 한 줄 | 1440px·1024px 한국어에서 범위 문장의 client rect가 한 줄이고 horizontal overflow가 없다. |
| P1 | 반응형 | 768px·390px에서 문장은 자연 줄바꿈하고 map·node·page overflow가 없다. |
| P1 | 상호작용 유지 | load 정지, hover/focus 경로 강조와 reduced-motion 계약이 유지된다. |
| P1 | 접근성 | 세 route의 desktop·mobile에서 axe WCAG A/AA violation과 browser 오류가 없다. |
| P1 | 회귀 방지 | 정적 gate가 한 열 Hero, 5개 색 token, desktop nowrap·mobile normal과 기존 map 계약을 검사한다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: Hero action과 Access·Service anchor의 tab 순서·solid outline을 유지한다.
- 색상 외 표현: 01~05 숫자·제목·surface border와 top-down connector가 항상 보인다.
- contrast: layer index와 보조 label은 dark surface에서 axe contrast 기준을 통과해야 한다.
- 1440/1024px: copy 한 줄, service 4열·runtime 5열 또는 1024px 분할 전환을 확인한다.
- 768/390px: `white-space: normal`, 기존 button 1열과 map compact grid, footer 연결을 확인한다.
- motion: layer identity 색은 정적이고 기존 hover/focus signal만 animation을 사용한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 배치·한 줄·색상 차이·overflow·contrast를 계산 style과 실제 브라우저로 직접 검증할 수 있다.

## 8. 구현 후 검증

- UI 자동 테스트: `/`, `/ko/`, `/en/`의 1440×1000, 1024×900, 768×900,
  390×844에서 HTTP·언어·DOM 순서·계산 style·line rect·grid·overflow를 검사했다. 1440px과
  1024px의 한국어 범위 문장은 1줄, 390px에서는 2줄이고 page·map·node overflow는 모두 0이다.
  5개 index·border 계산 색은 모두 고유하며 axe WCAG A/AA violation과 console·page·request 오류는
  0건이다.
- 상호작용: 초기 route animation은 `none`, storage hover·keyboard focus는 `pcv-arch-flow-y`와
  해당 node만 강조하며 pointer leave 뒤 `none`으로 복원된다. reduced motion은 transform `none`,
  duration `1e-05s`, iteration `1`을 유지한다.
- 정적 build·저장소 gate: `npm run check`, `scripts/check_design_md.py`,
  `tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `scripts/strip_source_comments.py --check`, `git diff --check` 통과.
- 변경 후 캡처/hash:
  - desktop: `.scratch/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors/after-ko-desktop.png`
    — `f50365d669fc5c9d2e5ec08e8a7fb9871b406bada61527ee5e3d9696c4c7549f`
  - 1024px: `.scratch/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors/after-ko-tablet-wide.png`
    — `8f3b7fa3ddb8b284a6ce60f31aee6e3b3344c521cb2f9aa3979288bb41dc5b53`
  - mobile: `.scratch/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors/after-ko-mobile.png`
    — `ca9f052714ef042d21cf7218e705faef6cb7aa1cad7e8b1747f9f9970f480061`
  - hover: `.scratch/ui-reviews/2026-08-24-landing-architecture-bottom-layer-colors/after-ko-desktop-hover.png`
    — `f105f521c13356889d1e98cd73f59cfd685e42e4540bab80755f649c6e996526`
- 로컬 시각 검토: desktop에서 copy 뒤 전체 폭 map, 5색 계층의 억제된 경계, 1024px 2행 전환,
  390px compact grid와 footer까지 직접 확인했다.
- 잔여 위험: 없음. 768px은 줄바꿈을 허용하지만 현재 문장 길이에서는 한 줄로 들어가며,
  더 긴 번역이 들어오면 정상적으로 자연 줄바꿈한다.
