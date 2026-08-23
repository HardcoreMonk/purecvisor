# 공개 랜딩 Single Edge 경로 탐색기 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/`의 Hero와 Single Edge architecture figure
> **선행 리뷰:** `2026-08-24-landing-architecture-readability-themes.md`

## 1. 제품 맥락과 목표

- 사용자: 첫 화면에서 단일 노드 제어면의 핵심 구조를 이해하고 필요한 운영 가이드로 이동하는
  운영자와 기술 의사 결정자
- 핵심 작업: Access에서 `purecvisorsd`를 거쳐 선택 capability의 runtime adapter와 Linux host
  자원까지 이어지는 한 경로를 pointer, touch와 keyboard로 탐색한다.
- 문제: 현재 figure는 20개 node를 거의 같은 우선순위로 동시에 보여준다. 390px에서 map 높이가
  1,537px이고 hover 경로 강조는 touch에서 탐색 상태로 유지되지 않는다.
- 목표: 5계층과 7개 정본 문서 link를 유지하면서 `purecvisorsd`와 선택 경로의 위계를 강화하고,
  mobile 기본 map 높이를 줄인다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 운영 URL: `https://purecvisor.site/ko/`
- 기존 light·dark desktop·mobile 캡처와 hash는 선행 리뷰의 구현 후 검증을 승계한다.
- 추가 측정: 1440px map 911px, 390px map 1,537px, 20개 node와 7개 link, 가로 overflow 0,
  light·dark Axe WCAG A/AA violation 0이다.

## 3. Refero 연구와 reference lock

| 역할 | 출처 | ID | 채택 규칙 |
|---|---|---|---|
| Primary canvas·density | Oxide Computer Company | `57399d2f-b94d-48df-a573-2c423077b16c` | Carbon/graphite surface, 얇은 border, 기술 diagram과 sans/mono 역할 분리 |
| 선택 telemetry | Eraser | `66b9ff99-f7f2-480c-97e9-42718e49cb97` | neon은 diagram의 선택 edge·icon·divider에만 사용하고 CTA·대면적 fill에는 사용하지 않음 |
| figure typography | PlanetScale | `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | architecture figure 안의 제목·control·node·기술 label 전체에 mono 역할 적용 |
| figure geometry | Tailscale | `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 최상위 hero information card 32px, 선택 route panel 16px, light의 매우 낮은 elevation |

Reference lock:

- Primary direction: Oxide의 정돈된 command-center 구조와 PureCVisor light/dark surface
- Preserve: 5계층, `purecvisorsd` 중심성, 7개 정본 link, 최소 12px label, light/dark와 reduced motion
- Borrow only: Eraser의 선택 edge와 `6px 6px 0` offset shadow, PlanetScale의 figure-local mono,
  Tailscale의 32px outer/16px route geometry
- Role rule: neon은 선택한 기술 경로만 나타내고 status·CTA 의미를 대체하지 않는다. 큰 radius와
  offset shadow는 architecture artifact 바깥의 제품 card로 확산하지 않는다.
- Media: 외부 이미지 없이 code-native HTML·CSS·SVG diagram을 유지한다.
- Reject: neon aura·gradient hero, 모든 card의 큰 radius, 모든 node의 offset shadow, 외부 font·asset

## 4. 결정 ledger

| 결정 | 근거 | 적용 이유 |
|---|---|---|
| Hero의 실제 가치 문장을 H1으로 승격 | 사용자 승인, screen hierarchy 조사 | 작은 version label이 H1인 의미·시각 계층을 바로잡음 |
| capability를 4개 선택 control과 별도 guide link로 분리 | 사용자 승인, touch 제약 | tap은 경로 선택, 보조 arrow link는 문서 이동이라는 역할을 분명히 함 |
| runtime·host는 선택 경로 node만 기본 표시 | 현재 mobile 측정, PlanetScale blueprint | 5계층을 유지하면서 inventory 높이를 줄이고 관계를 먼저 읽게 함 |
| 선택 route 전체를 16px panel과 offset shadow로 묶음 | Eraser Shadowed/Highlight Card | shadow를 한 곳에 제한해 선택된 관계만 기억하게 함 |
| outer figure에 32px radius 적용 | Tailscale Hero Information Card | 반복 card가 아닌 하나의 설명 media frame으로 사용 |
| inactive layer와 node를 neutral hairline으로 후퇴 | Oxide, PureCVisor token 역할 | 계층 5색보다 선택 경로와 control plane을 우선함 |

## 5. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | `/`, `/ko/`, `/en/`에 5계층, 3개 Access link와 4개 capability guide link가 유지된다. |
| P0 | capability control은 `aria-pressed`를 갱신하고 선택 label, runtime과 host node를 같은 경로로 바꾼다. |
| P0 | light·dark와 1440·1024·768·390px에서 Axe WCAG A/AA violation, 겹침과 가로 overflow가 0이다. |
| P1 | 390px 기본 map 높이는 1,000px 이하이고 inactive runtime·host node는 접근성 tree에서도 제외된다. |
| P1 | outer 32px radius, route 16px radius, 선택 route `6px 6px 0` shadow가 계산 style에서 확인된다. |
| P1 | neon 4색은 path token과 선택 edge에만 사용되고 CTA·상태색·대면적 background로 확산되지 않는다. |
| P1 | keyboard focus, pointer 선택, touch click, reload 기본 경로와 reduced motion을 검증한다. |

## 6. 구현 후 검증

### 구현

- `site/src/content/docs/index.mdx`와 `site/src/content/docs/en/index.mdx`에서 version label을
  eyebrow로 내리고 실제 가치 문장을 H1으로 승격했다.
- capability 4개를 `aria-pressed` path control과 별도 guide link로 분리하고, 선택 label과
  runtime·host node를 같은 `data-active-path` 상태로 전환한다.
- `site/src/components/ArchitectureMapScript.astro`가 pointer·touch click과
  `ArrowLeft`·`ArrowRight`·`ArrowUp`·`ArrowDown`·`Home`·`End` keyboard 탐색, roving
  `tabindex`와 live output을 관리한다.
- `site/src/styles/custom.css`에 figure-local mono, 32px outer radius, 16px active route,
  선택 경로별 neon edge와 단 하나의 `6px 6px 0` offset shadow를 적용했다. 390px에서는
  중복 secondary copy를 줄이고 04→05의 세로 관계를 유지해 지도를 압축했다.
- `site/scripts/check-site.mjs`와 `docs/PUBLIC_DOCUMENTATION_SITE.md`를 같은 release 단위의
  구조·시각·interaction 회귀 계약으로 갱신했다.

### 자동·실브라우저 결과

- `cd site && npm run check`: PASS, 26 pages와 86 files artifact 검증
- `make check-public-comments`: PASS, first-party source/JavaScript comments 0과 공개 정책 통과
- `python3 scripts/check_design_md.py`: PASS
- 실브라우저 24개 조합: `/`, `/ko/`, `/en/` × light/dark × 1440/1024/768/390px
- Axe WCAG A/AA violation 0, console·page·request error 0, page·map·route·control overflow 최대 0px
- 계산 style: outer radius 32px, active route radius 16px, shadow offset 6px/6px/0,
  figure font family 1종 mono, 최소 label 12px
- map 높이: 1440px 1,018px, 1024px 1,141px, 768px 1,141px, 390px 한국어 980px·영어 996px
- 네 path 모두 pointer 선택, 단일 pressed/tab stop, live label, runtime·host node mapping 통과
- keyboard 순환 `workloads → storage → vpc → workloads → vpc`와 reduced motion 통과

최종 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| Korean light desktop | `de0a95c3488d7ca73a059dfd44dabf8b42667ad2c23a5d3dd5b4d1648a8ce1df` |
| Korean dark desktop | `4ee4338cf5c3c80801d6cc47775227608e47358ce800b693b6309cf7fc3ae596` |
| Korean light mobile | `8a855605a1086dd63c9cfe95e86e63e28f5ce2428ac638ea9e06fc59d666b8fb` |
| Korean dark mobile | `2331a071a672e82324d32a1375e381e3cb0aede6b58d829807ae432faaed7560` |

P0·P1 수용 기준을 모두 충족했으며 열린 P2는 없다.
