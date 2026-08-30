# 랜딩 아키텍처 SVG 노출 제거 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LIVE-PASS — GitHub Pages와 custom domain 검증 완료
> **대상:** `/`, `/ko/`, `/en/` landing Hero
> **관련 리뷰:** `2026-08-25-landing-service-architecture-source-svg.md`,
> `2026-08-30-overview-architecture-rollover-flow.md`

## 1. 제품 맥락과 목표

- 사용자: 제품 범위와 시작 경로를 빠르게 확인하려는 Single Edge 운영자와 신규 방문자
- 핵심 작업: H1과 한 문장 설명을 읽고 5분 퀵스타트 또는 전체 운영 가이드로 이동한다.
- 변경 목표: landing Hero 아래의 전체 SVG figure·범례·확대 link·설명을 제거해 첫 화면을 제품
  소개와 두 action에 집중시킨다.
- 비범위: header navigation, H1·lead·CTA·배포 note 변경, 시작하기의 두 아키텍처 탭, SVG asset
  삭제·수정, 새 이미지·card·문서 directory 추가

설계 brief는 “Single Edge 신규 방문자를 위한 web landing 정리”다. 목표는 상세 구조를 읽기 전에
제품 범위와 시작 action을 파악하게 하는 것이며, 톤은 현행의 흰 canvas·soft gray·teal accent를
유지한다. 주요 위험은 architecture 접근 경로가 사라지는 것이지만, 상세 지도와 설명은
`/ko/getting-started/overview/#12-아키텍처-개요`가 계속 소유한다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-30, live custom domain
- 캡처 경로: `.scratch/ui-reviews/2026-08-30-landing-architecture-removal/`
- 현재 동작: 세 landing이 full architecture SVG를 eager load하고 figure, 7개 layer 범례, 확대 link
  2개와 장문 note를 표시한다.
- 실측: 한국어 desktop Hero `2577.421875px`, mobile Hero `1533.78125px`; 영어 desktop Hero
  `2577.421875px`, mobile Hero `1551.78125px`; 각 route의 diagram request 1개, overflow 0,
  Axe WCAG A/AA 위반 0

| 현재 캡처 | SHA-256 |
|---|---|
| `before-ko-desktop.png` | `bc33238154341b03629d35cd44ec260fe392d8f014d9a1b47e7ff545e74b26d4` |
| `before-ko-mobile.png` | `2cd5bf5b311ad318d930781cac900942761081844ed1852846276599a71b4e04` |
| `before-en-desktop.png` | `1c361d6636c2f9c9f4ff450871bbcb4ca565d4e6d185f18b8faa60d78470f1e9` |
| `before-en-mobile.png` | `f56e911a2ef811b13c0bd3847d05004e381da56baa277dcdbbee7d44de87d37a` |

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 명시 요청 | 2026-08-30 | landing의 SVG architecture image 노출 삭제 | 채택 — 최우선 범위 |
| Refero OpenAI Developers style | `44317718-1e56-45e0-8de3-7ede70f34349` | image-light hero와 단일 column text hierarchy가 개발자 진입점을 명확히 함 | 채택 — 기존 copy·CTA만 유지 |
| Refero Tailscale style | `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 기술 media는 설명 역할이 있을 때만 사용하고 layout은 절제된 정보 계층을 우선 | 채택 — 상세 SVG를 overview에 한정 |
| Refero Vectary style | `91b6dc03-35f1-461a-80b6-0b2473c05f5a` | 복잡한 imagery 없이 typography·spacing·contrast만으로 명료한 진입 화면 구성 | 부분 채택 — 현행 token 안에서 media 제거 |
| Refero HTTPie docs screen | `a01522c6-b9c9-472c-b753-ced6e8d945e2` | developer landing의 media는 구체적 문서 선택을 보조할 때 사용 | 기각 — 대체 code panel은 요청 범위 밖 |
| Refero Cursor docs screen | `e546876e-ca6b-4925-8319-9f24f8f29326` | 상세 screenshot과 구조 설명은 documentation reader 맥락에서 제공 | 채택 — architecture는 시작하기 reader에 유지 |

flow 조사는 사용하지 않았다. 사용자 상태를 바꾸는 다단계 여정이 아니라 한 landing의 설명 media를
제거하는 정보 계층 변경이다.

## 4. Reference lock과 결정

**Primary direction:** 현행 PureCVisor의 typography-first 단일 Hero

**반드시 보존:** header disclosure, 흰/짙은 theme canvas, product eyebrow, H1, lead, CTA 2개,
deployment note, 좌측 정렬, 1920·390px 무가로 overflow, 44px 이상 action target

**Media strategy:** landing media 없음. 전체 architecture SVG와 interactive rollover는
시작하기 overview에만 유지한다.

### 채택

- 한국어 source와 영어 source에서 `<figure>` 전체를 삭제한다.
- figure 안의 title, status, 확대 link, 7개 범례, SVG `<img>`, 장문 note를 함께 제거한다.
- landing 전용으로 남는 layer legend token·selector와 breakpoint rule을 제거한다.
- build gate가 세 landing에서 diagram markup과 `/assets/diagrams/` request가 없는 상태를 요구하게 한다.

### 기각

- 빈 figure shell, 대체 illustration, architecture thumbnail과 새로운 feature card를 추가하지 않는다.
- H1·lead를 중앙 정렬하거나 Hero 높이를 viewport에 강제로 맞추지 않는다.
- SVG asset과 시작하기의 direct/NGINX architecture 탭은 삭제하지 않는다.
- architecture 정보를 landing의 장문 prose로 옮기지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | landing SVG 노출 | `/`, `/ko/`, `/en/`에 architecture figure·legend·diagram image·diagram link가 0개다. |
| P0 | 네트워크 노출 | 세 landing 진입 중 `/assets/diagrams/` request가 0개다. |
| P0 | 상세 문서 보존 | overview의 direct/NGINX 탭, 두 SVG와 rollover interaction gate가 그대로 통과한다. |
| P1 | Hero 계층 | eyebrow·H1·lead·CTA 2개·deployment note의 문구와 순서가 유지된다. |
| P1 | 반응형 | 1920·1280·390px에서 가로 overflow 0, action target 44px 이상이다. |
| P1 | 접근성·안정성 | H1 1개, Axe WCAG A/AA 위반 0, console·page·request 오류 0이다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 두 CTA와 header navigation의 기존 focus contract를 유지한다.
- 색상 외 상태 표현: 삭제 작업이므로 새 상태를 만들지 않는다. CTA는 border·fill·focus outline을 유지한다.
- loading/empty/error/disabled: eager SVG request 자체가 없어 media loading·error 상태도 사라진다.
- 1024/768/480px: Hero는 단일 text column과 자연 높이를 사용하고 mobile action은 기존처럼 세로 배치한다.

## 7. 정량 검증

Attention Insight는 사용하지 않는다. 핵심 action의 위치나 여러 대안의 시선 분포를 비교하는 작업이
아니라 사용자가 지정한 media 제거이며 DOM·request·viewport 실측으로 수용 여부를 직접 판정한다.

## 8. 구현 후 검증

### 8.1 구현 결과

- root 한국어 source와 영어 source에서 architecture `<figure>` 전체를 제거했다. build가 복제하는
  `/ko/`에도 같은 결과가 적용된다.
- landing에서만 사용하던 7개 layer color token, legend·key·swatch selector와 두 breakpoint rule을
  제거했다. overview가 사용하는 figure·canvas·image·tab·interaction CSS는 유지했다.
- site gate는 세 landing의 figure·diagram image·diagram link와 architecture 문구를 금지하고,
  eyebrow·H1·lead·CTA·deployment note를 계속 요구한다.
- 공개 운영 기준은 상세 아키텍처의 소유자를 시작하기 `1.2 아키텍처 개요`로 변경했다.

### 8.2 로컬 브라우저 검증

Astro production preview에서 `/`, `/ko/`, `/en/` × 1920×1080, 1280×900, 390×844 ×
light/dark의 18개 조합을 검사했다.

| 항목 | 결과 |
|---|---|
| architecture DOM | figure·source·legend·canvas·image 모두 route별 0 |
| architecture link/request | diagram link 0, `/assets/diagrams/` request 0 |
| Hero 높이 | desktop `515.6875px`, 1280px `502.515625px`, mobile `514.984375px` |
| Hero 콘텐츠 | 언어별 H1 유지, CTA 2개, target 높이 48px |
| 반응형 | 18개 조합 모두 가로 overflow 0 |
| 접근성·오류 | Axe WCAG A/AA 위반 0, console·page·request 오류 0 |
| overview 보존 | direct 31 node·40 edge·8 layer·rollover edge 4, NGINX 32·41·9, 오류 0 |

| 변경 후 캡처 | SHA-256 |
|---|---|
| `after-local-ko-light-desktop.png` | `ceecabdd933229cd4c0edcc383f9ffb7add4bc3fe80c00dd96185ede1919eea0` |
| `after-local-ko-dark-desktop.png` | `aead265e5c55a9d92ab3ce902f6e0301f1b0244a970b0eafcfb3a981c72106c9` |
| `after-local-ko-light-mobile.png` | `51ae3f1c7941bb4864777370ba5ab971506e4a4ae413e05df18b5612637e2ab7` |
| `after-local-ko-dark-mobile.png` | `31b591f2aa6ae817513b1d3d556092abc2701ebe7281f2b3099f6d106c56523c` |
| `after-local-en-light-desktop.png` | `8780e356cd594e8aedbef1a247794e929dbdbe29fc70bbdb8d3a2792d06f3c6e` |
| `after-local-en-dark-desktop.png` | `c8183c450b2127d9afcd0f6a0ae39247b86b4f24c47ea071199c1a042bf1c93b` |
| `after-local-en-light-mobile.png` | `44c879484202f0b2234edb49994bf462bc92001d78b7f53446041b72c4a8f711` |
| `after-local-en-dark-mobile.png` | `e34243cedc21972bcbb5995b8bda2e570cba9b4dbb0bd4b19ab741fa5a8f2914` |

### 8.3 저장소 게이트

- `npm run check`: PASS — 27 page, Pages artifact 92 file
- `make single`: PASS
- `make test`: PASS — g_test path 1,370개, audit startup 5/5
- `make check-all`: PASS — 공개 계약 gate 38개
- `make release`: PASS
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS — source
  SHA-1 `45519e8e`, cache `pcv-ui-v50c54abc`
- `python3 scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`: PASS
- `make check-public-comments`, `python3 scripts/strip_source_comments.py --check`: PASS — 공개 source
  comment 0
- site·interaction·bundle JavaScript syntax와 `git diff --check`: PASS

### 8.4 Pages 배포 검증

- source commit: `4330260492463f1ac823ac2469b35dbd92158e23`
- GitHub Actions: [`pages` run 33302637830](https://github.com/HardcoreMonk/purecvisor/actions/runs/33302637830)
  — build 24초, deploy 9초, 두 job 모두 PASS
- live 대상: `https://purecvisor.site/`, `/ko/`, `/en/`

custom domain에서 로컬과 같은 18개 조합을 다시 실행했다. 모든 route·theme·viewport에서
architecture DOM·diagram link·`/assets/diagrams/` request 0, CTA 2개·48px, overflow 0,
Axe WCAG A/AA 위반 0, console·page·request 오류 0이었다. Hero 높이는 desktop
`515.6875px`, 1280px `502.515625px`, mobile `514.984375px`로 로컬과 일치했다.

시작하기 overview도 direct 31 node·40 edge·8 layer·rollover edge 4, NGINX 32 node·41 edge·9
layer와 오류 0을 다시 확인했다. live 대표 캡처 8개의 SHA-256은 8.2의 local hash와 각각
pixel-identical하므로 최종 판정은 `LIVE-PASS`다.
