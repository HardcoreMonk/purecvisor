# 공개 랜딩 기능 지도 아이콘·Flow Motion 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge capability map 상호작용
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046,
> `docs/ui-reviews/2026-08-24-landing-capability-map.md`

## 1. 제품 맥락과 목표

- 사용자: 첫 화면에서 Single Edge의 제공 기능을 비교하고 관련 운영 가이드로 이동하는 운영자와
  기술 의사 결정자
- 핵심 작업: 워크로드·스토리지·네트워크 패브릭·가상 네트워크를 시각적으로 구분하고 관심
  영역의 상세 문서로 이동한다.
- 변경 목표: 사용자 요청에 따라 기능 지도에 서비스별 아이콘을 추가하고 mouse rollover 시
  접근층 → 제어면 → 서비스로 이어지는 flow가 반응하는 역동적인 시각화를 제공한다.
- 비범위: capability 명칭·공개 범위, Hero copy·action, 문서 directory와 별도 animation
  library 도입은 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map-motion/before-ko-hero.png`
- SHA-256: `841a689524a489aea0b0cde227d433a44fa466e542f957ee1724dd59bbe22bc0`
- 현재 동작·데이터 계약: 4개 capability card가 동일한 텍스트·chip 구조로 정적으로 표시된다.
  pointer hover와 keyboard focus 상태, 서비스 icon, 직접 문서 link와 motion이 없다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | 서비스 icon과 rollover animation flow로 시각화 개선 필요 | 채택, interaction 변경의 직접 근거 |
| 기존 기능 지도 리뷰 | `docs/ui-reviews/2026-08-24-landing-capability-map.md` | 4개 기능 영역과 공개 capability, 2×2·1열 반응형 구조 승인 | 정보 구조·색상 token·공개 경계 유지 근거로 채택 |
| Vercel Infrastructure | [`d994631b-6d7a-4e07-8aa0-07788706b57d`](https://refero.design/pages/d994631b-6d7a-4e07-8aa0-07788706b57d) | 어두운 인프라 소개에서 icon을 가진 기능 card와 제한된 accent로 영역을 구분함 | 4개 서비스별 선형 icon·동일 card grammar 근거로 채택 |
| Column Infrastructure | [`9bfcb6a5-095c-4d23-9aaf-06e17ad454d7`](https://refero.design/pages/9bfcb6a5-095c-4d23-9aaf-06e17ad454d7) | icon·제품명·Learn more를 한 card에 결합해 기능 이해와 상세 진입을 연결함 | capability card 전체를 운영 가이드 link로 만드는 근거로 채택 |
| FlowMapp Sitemap | [`1a7e5c6f-7384-4b47-9080-53838a392a6b`](https://refero.design/pages/1a7e5c6f-7384-4b47-9080-53838a392a6b) | parent와 child node를 균형 잡힌 연결선으로 보여 줘 방향과 관계를 즉시 읽게 함 | 접근층→제어면→기능 영역의 connector를 flow animation 대상으로 쓰는 근거로 부분 채택 |

새 브랜드 style을 도입하지 않고 기존 dark/teal token 안에서 icon·연결선·상태 표현만 확장한다.

## 4. 결정

### 채택

- 4개 capability card에 외부 asset이 아닌 `currentColor` 기반 inline SVG 선형 icon을 추가한다.
  - 워크로드: VM window
  - 스토리지: disk stack
  - 네트워크 패브릭: bridge node
  - 가상 네트워크: VPC topology
- card 전체를 대응 운영 가이드 link로 바꾼다. 워크로드는 VM, 스토리지는 storage, 두
  network card는 networking 정본 route로 연결한다.
- pointer hover와 `:focus-visible`에서 같은 상태를 제공한다.
  - 선택 card의 border·surface·icon·arrow를 teal로 강조하고 2px 위로 이동
  - icon 내부 flow stroke를 한 번 그리는 motion 적용
  - 두 connector의 signal gradient를 반복 이동하고 `purecvisorsd` core를 약하게 강조
  - 선택하지 않은 card의 정보는 숨기거나 흐리게 만들지 않음
- page load에서는 animation을 재생하지 않고 실제 hover·focus 동안만 flow를 시작한다.
- `prefers-reduced-motion: reduce`에서는 animation을 1회 0.01ms로 제한하고 transform을 제거한다.

### 기각

- Lottie, canvas, WebGL이나 animation JavaScript dependency를 추가하지 않는다. 정적 사이트
  bundle과 code-native 시각 언어에 비해 과도하다.
- icon을 장식 emoji나 제3자 logo로 만들지 않는다. 플랫폼·운영체제별 모양 차이와 브랜드
  오인을 피한다.
- hover에서 capability 설명을 교체하거나 popover를 띄우지 않는다. touch와 keyboard에서
  정보 불일치가 생긴다.
- page load부터 무한 pulse를 재생하지 않는다. Hero 판독을 방해하고 motion 민감 사용자에게
  불필요한 움직임을 준다.

## 5. link mapping

| 영역 | route |
|---|---|
| VM·컨테이너 / Workloads | `/ko/workloads/virtual-machines/` |
| 스토리지 / Storage | `/ko/infrastructure/storage/` |
| 네트워크 패브릭 / Network Fabric | `/ko/infrastructure/networking/` |
| 가상 네트워크 / Virtual Network | `/ko/infrastructure/networking/` |

## 6. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 서비스 시각 구분 | root·`/ko/`·`/en/`에 서로 다른 4개 inline SVG icon과 4개 유효 link가 있다. |
| P0 | rollover flow | card hover 시 connector signal, core highlight, card·icon motion이 실행되고 pointer 이탈 시 기본 상태로 복귀한다. |
| P0 | keyboard 동등성 | Tab focus에서 hover와 같은 flow·강조 상태와 읽을 수 있는 focus ring을 제공한다. |
| P1 | motion 안전 | page load에서 animation이 없고 reduced motion에서 animation·transform이 사실상 제거된다. |
| P1 | 반응형 | 1440px 2×2, 390px 1열에서 icon·text·chip이 겹치지 않고 overflow가 없다. |
| P1 | 회귀 방지 | 정적 gate가 icon·link·keyframes·focus·reduced-motion 계약을 검사한다. |

## 7. 접근성·반응형·상태 검토

- keyboard/focus: card가 실제 link가 되어 Tab 순서에 포함되고 `:focus-visible` outline과 hover
  동등 상태를 제공한다.
- icon 의미: SVG는 `aria-hidden="true"`이고 card의 실제 text가 link name을 제공한다.
- 색상 외 표현: 선택 상태는 색상뿐 아니라 위 이동, icon stroke motion, arrow 이동과 focus
  outline으로 구분한다.
- touch: hover에 의존하는 추가 정보가 없고 card tap은 바로 정본 문서로 이동한다.
- loading/empty/error/disabled: 정적 link map이므로 해당 상태는 없다.
- 1024/768/480px: card heading의 icon·title·arrow 정렬, 긴 Security Groups chip과 map/page
  overflow를 확인한다.

## 8. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자 지정 interaction과 접근성 상태 검증이 핵심이며 정적 시선 heatmap이 motion
  품질을 판정하지 못한다.

## 9. 구현 후 검증

- UI 자동 테스트: `npm run check`가 26개 HTML과 총 86개 Pages artifact, 내부 link,
  4개 inline SVG icon·capability link, hover/focus keyframe과 reduced-motion 계약을 검증했다.
- 저장소 게이트: `python3 scripts/check_design_md.py`,
  `bash tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `python3 scripts/strip_source_comments.py --check`, `git diff --check`가 통과했다.
- Chromium `1440 × 1000`과 `390 × 844`: `/`, `/ko/`, `/en/` 모두 4개 link·서로 다른
  4개 icon이 있고 icon은 `aria-hidden`, link name은 실제 card text로 제공됨을 확인했다.
- page load에서 connector animation name은 `none`이다. 첫 card hover 시
  `pcv-map-flow-x`, `pcv-map-flow-y`, `pcv-map-card-signal`, `pcv-map-icon-draw`가 실행되고
  card는 약 2px 위로 이동하며 core glow가 적용됐다.
- pointer 이탈 260ms 뒤 connector animation과 card transform이 `none`으로 복귀했다.
- keyboard Tab으로 capability card에 진입하면 `:focus-visible`이 참이고 solid outline과
  `pcv-map-flow-x`가 함께 적용돼 hover와 동등한 상태임을 확인했다.
- `prefers-reduced-motion: reduce`에서는 card·icon transform이 `none`, connector·card motion은
  `0.01ms`·1회로 제한됐다.
- 모든 route·viewport에서 axe 위반, browser console·page·request 오류, map·page horizontal
  overflow는 0이다.
- desktop 기본 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map-motion/after-ko-desktop.png`
  — `44869071e09942ecf03b15ee7b50466fcc21a209089ad89758872c8f1a7cb058`
- desktop hover 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map-motion/after-ko-desktop-hover.png`
  — `1f6bfe05c12a9d4d41f87af50342157b49d42b91061423a2e5ea551531488722`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map-motion/after-ko-mobile.png`
  — `5711ed7ace8a160938f084c9fd37212cfda30df403b1c88df0f34c82556e80`
- 잔여 위험: `:has()`를 지원하지 않는 구형 browser에서는 card 자체 hover·focus는 유지되지만
  상위 connector flow가 생략될 수 있다. 현재 지원 browser의 계산된 style 검증과 정적 gate가
  interaction 회귀를 차단한다.
