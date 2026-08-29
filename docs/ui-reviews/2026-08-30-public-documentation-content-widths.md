# 공개 문서 의미 기반 콘텐츠 폭 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LIVE-PASS — 로컬·GitHub Pages·custom domain 시각·접근성 검증 완료
> **대상:** Starlight 문서 reader의 일반 본문, 표, code block과 아키텍처 자료
> **관련 결정:** ADR-0047

## 1. 설계 brief

PureCVisor 공개 문서에서 일반 문장의 안정적인 줄 길이는 보존하면서 열이 많은 표, 긴 code와
아키텍처 자료의 판독성을 높인다. 모든 본문을 일괄 확장하지 않고 콘텐츠의 역할에 따라 최대 폭을
다르게 적용한다. 기존 Starlight header, 좌·우 navigation, typography, 색상과 모바일 동작은
변경하지 않는다.

## 2. 변경 전 상태

- `--sl-content-width: 50rem` 하나가 일반 문장, heading, 표, code block에 모두 적용되었다.
- 1920px desktop의 데이터베이스 아키텍처 page에서 Markdown 본문과 표의 가용 폭은 800px였다.
- 이 문서의 표 52개는 내부 가로 scroll을 지원하지만 여러 열의 문장이 많이 줄바꿈되어 세로
  탐색 비용이 컸다. code block 9개도 같은 50rem 상한을 사용했다.
- landing 서비스 아키텍처는 이미 75rem shell과 SVG 원본 확대 link를 사용했다.

## 3. 연구 근거와 reference lock

현재 PureCVisor Starlight shell, typography와 색상을 primary reference로 고정했다. 일반 문장의
50rem 줄 길이와 `최신 릴리스 기준`의 명시적 세 줄 계약을 보존하고 구조 자료만 선택적으로
확장한다.

| 근거 | 확인한 원칙 | 이번 적용 |
|---|---|---|
| Refero의 OpenAI Developers, PlanetScale, SST style과 Resend·Mapbox·Doppler 문서 화면 | 본문은 좁고 안정적인 column에 두고 구조 정보와 API 자료에 더 넓은 면적을 배정 | 전역 확대 대신 prose·technical·architecture 폭을 분리 |
| [W3C WCAG 2.2 Visual Presentation](https://www.w3.org/WAI/WCAG22/Understanding/visual-presentation.html) | 읽기 문장은 과도하게 긴 행을 피하고 사용자가 확대한 상태에서도 읽을 수 있어야 함 | 일반 본문 50rem 유지 |
| [W3C WCAG 2.2 Reflow](https://www.w3.org/WAI/WCAG22/Understanding/reflow.html) | 작은 viewport에서 양방향 page scroll을 강제하지 않아야 함 | 폭 값은 상한으로만 사용하고 표·code 내부 overflow 유지 |
| [Astro Starlight CSS variables](https://starlight.astro.build/reference/css-variables/) | `--sl-content-width`로 reader canvas를 확장할 수 있음 | 바깥 canvas만 75rem으로 열고 자식 역할별 상한 적용 |
| [GitHub Primer Prose](https://primer.style/product/components/prose/)와 [VitePress 기본 theme source](https://github.com/vuejs/vitepress/blob/main/src/client/theme-default/styles/components/vp-doc.css) | 문장용 기본 폭과 필요 시 넓히는 변형을 분리하는 문서 UI 패턴 | 50rem prose를 기본값으로 두고 명시적인 wide tier 제공 |

외부 사례의 logo, 색상, 문구와 수치를 복제하지 않는다. PureCVisor의 실제 콘텐츠 길이와 현재
reader geometry를 기준으로 확장 수치를 결정했다.

## 4. 폭 수치 분석

1920px viewport의 데이터베이스 아키텍처 page에서 52개 표 전체의 렌더링 높이를 비교했다.

| 최대 폭 | 전체 표 높이 | 50rem 대비 변화 | 해석 |
|---:|---:|---:|---|
| 50rem | 20,819px | 기준 | 일반 본문과 동일한 기존 상태 |
| 60rem | 19,335px | -7.1% | 일부 긴 열의 줄바꿈 감소 |
| 64rem | 19,195px | -7.8% | 추가 이득이 작음 |
| 68rem | 18,803px | -9.7% | 표 머리글과 긴 셀에서 두 번째 유효 변곡점 |
| 72rem | 18,607px | -10.6% | 68rem 이후 한계 이득 0.9%p |
| 75rem | 18,467px | -11.3% | 기술 자료에는 넓고 prose와의 대비가 커짐 |

첫 핵심 표는 50rem에서 높이 1,001px였고 68rem에서 777px로 22.4% 줄었다. 72rem부터는
749px로 거의 수렴했다. code fence 2,104줄의 길이는 95백분위 75자, 99백분위 104자였다.
68rem은 약 125개의 monospace 문자를 표시해 99백분위를 수용한다. 따라서 표·code는 68rem을
가독성과 공간 사용의 균형점으로 정했다.

서비스 아키텍처 SVG는 원본 폭 1,849.523px이며 50rem에서 약 43.3%, 68rem에서 약 58.8%,
75rem에서 약 64.9%로 축소된다. 원본 16px label은 각각 약 6.9px, 9.4px, 10.4px로 보이므로
아키텍처 tier는 75rem으로 두고 세부 라벨을 위한 원본 확대 link를 필수로 유지한다.

## 5. 결정 ledger

| 결정 | 이유 |
|---|---|
| 일반 본문·heading·pagination 최대 50rem | 문장 줄 길이와 현재 reader 위치를 보존 |
| 표·code block 최대 68rem | 전체 표 높이 9.7%, 핵심 표 높이 22.4%를 줄이는 측정상 변곡점 |
| figure·명시적 아키텍처 자료 최대 75rem | 축소된 SVG label의 inline 판독성을 높이고 기존 landing shell과 일치 |
| Starlight 바깥 canvas 최대 75rem | 역할별 자식 요소가 별도 page shell 없이 확장될 공간 확보 |
| 모든 폭을 `max-width`로 적용 | 1440px 이하에서 가용 폭으로 자연 축소하고 page overflow 방지 |
| 표의 내부 scroll·keyboard focus 유지 | 열 자체를 축소하거나 page 전체를 가로 scroll하지 않음 |
| 기존 landing은 새 reader selector에서 제외 | 승인된 Hero와 서비스 아키텍처 geometry 보존 |

## 6. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 1920px reader에서 일반 본문 50rem, 표·code 68rem, 아키텍처 75rem 상한이 계산된다. |
| P0 | `최신 릴리스 기준`의 세 문장은 기존 위치와 명시적 세 줄을 유지한다. |
| P0 | 1440px와 390px에서 page-level 가로 overflow가 없다. |
| P1 | 넘치는 표와 code는 요소 내부에서만 scroll하며 표의 keyboard focus가 유지된다. |
| P1 | landing 서비스 아키텍처의 75rem shell, 전체 SVG와 원본 확대 link가 유지된다. |
| P1 | light·dark theme, 좌·우 navigation과 pagination에 구조·접근성 회귀가 없다. |

## 7. 구현

- `--pcv-prose-width`, `--pcv-technical-width`, `--pcv-architecture-width` token을 각각
  50rem, 68rem, 75rem으로 정의했다.
- `--sl-content-width`는 가장 넓은 아키텍처 canvas를 예약하고 Markdown의 top-level 자식은
  기본 50rem으로 중앙 정렬한다.
- 표, Expressive Code와 직접 code block은 68rem, figure와 `.pcv-architecture-wide`는
  75rem까지 확장한다. `.pcv-technical-wide`를 구조 자료의 명시적 68rem opt-in으로 제공한다.
- page title과 pagination도 50rem으로 맞추고 `.pcv-landing`은 reader 선택자에서 제외했다.
- artifact gate가 세 token과 역할별 selector를 검사해 전역 50rem 회귀를 차단한다.

## 8. 로컬 구현 검증

- `cd site && npm run check`: PASS. 27개 page를 build하고 Pagefind 27개 HTML과 Pages artifact
  90개를 검사했다. 새 폭 token·selector, 기존 세 줄 릴리스 문장과 landing SVG 계약도 통과했다.
- `python3 scripts/check_design_md.py`, `make check-public-comments`, `git diff --check`: PASS.
- Chromium `1920×1080` light·dark에서 Markdown canvas 1,200px, 일반 본문·H1·pagination 800px,
  첫 표와 code block 1,088px를 확인했다. 52개 표 전체가 확장 폭 안에 표시되었다.
- Chromium `1440×1000`에서는 좌·우 navigation을 제외한 가용 폭 784px로 모든 tier가 축소되며
  page-level overflow는 0이었다. 첫 표를 포함한 2개 표만 요소 내부 scroll을 사용했다.
- Chromium `390×844`에서는 reader 폭 358px, page-level overflow 0, 내부 scroll 표 29개,
  focus 가능한 표 52개를 확인했다. 첫 표에 focus한 뒤 방향키로 `scrollLeft 0→80` 이동했다.
- 모든 측정 viewport에서 Axe WCAG A/AA 위반과 console·page·request 오류는 0이었다.
- `최신 릴리스 기준` 문단은 1920px에서 `x=536`, `width=800`과 시각적 세 줄을 유지했고,
  배포 전 custom domain의 위치·폭·줄 수와 일치했다.
- landing 서비스 아키텍처 figure는 1,200px, SVG image content는 border 안쪽 1,198px이며
  원본 SVG 확대 link와 page-level overflow 0을 유지했다.
- 로컬 캡처:
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/local-desktop.png`
    — `07981acc9b48b8997d1ebb0c15d96fdc09e703aa8709061c3a2a5098ad33d142`
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/local-mobile.png`
    — `8ce11e66fcafa1ef6045293ac4031b9d3dd6e18f34d31c86282203933095d6f8`
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/local-release.png`
    — `20cdaf6ae544538e6f615ae2345abc49379e1a51ed1c1bc9bb15fb0f0b178029`

## 9. GitHub Pages·custom domain 검증

- 구현 commit `a82d1f4d9eca2aec6352322a0497872f18700124`을 public `main`에 push했다.
- GitHub Pages run
  [`33281197096`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33281197096)의
  `npm ci`, `npm run check`, artifact upload와 deploy가 모두 성공했다.
- `https://purecvisor.site/ko/development/database-architecture/`는 HTTP 200,
  `text/html; charset=utf-8`, 한국어와 정확한 canonical을 반환했다.
- live Chromium의 `1920×1080` light·dark, `1440×1000`, `390×844` 측정값은 로컬과
  동일했다. 일반 본문·H1·pagination 800px, 표·code 1,088px, 바깥 canvas 1,200px가 적용되고
  작은 viewport에서는 각각 784px와 358px로 축소되었다.
- 모든 viewport에서 page-level overflow, Axe WCAG A/AA 위반과 console·page·request 오류는
  0이었다. mobile 표 52개가 focus 가능하고 첫 표는 방향키로 `scrollLeft 0→80` 이동했다.
- live `최신 릴리스 기준`은 `x=536`, `width=800`, line-height 28px와 시각적 세 줄을 유지했다.
  landing 서비스 아키텍처도 1,200px figure와 원본 확대 link를 유지했다.
- live 캡처:
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/live-desktop.png`
    — `3594f1b9d4b30ffe25042d31b4cc0727f4574429c5f7a1a0d87b870973487ff6`
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/live-mobile.png`
    — `3c2358471031b16a298cadec80903aee85aafda87e1cdd96a205d363e1fa6910`
  - `.scratch/ui-reviews/2026-08-30-public-documentation-content-widths/live-release.png`
    — `20cdaf6ae544538e6f615ae2345abc49379e1a51ed1c1bc9bb15fb0f0b178029`

모든 수용 기준을 충족했으므로 이 리뷰를 **LIVE-PASS**로 확정한다.
