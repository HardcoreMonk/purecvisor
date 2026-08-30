# 시작하기 아키텍처 연결 흐름 롤오버 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LIVE-PASS — GitHub Pages와 custom domain 검증 완료
> **승인:** 2026-08-30 사용자 명시 승인
> **대상:** `/ko/getting-started/overview/#12-아키텍처-개요`
> **관련 리뷰:** `2026-08-30-overview-architecture-completeness.md`,
> `2026-08-30-overview-architecture-tls-mode-tabs.md`

## 1. 제품 맥락과 목표

- 사용자: 전체 구조를 읽은 뒤 특정 서비스 레이어나 컴포넌트가 어느 요청·상태 경로와 직접
  연결되는지 빠르게 추적하려는 Single Edge 운영자와 개발자
- 핵심 작업: 현재 TLS 모드의 SVG에서 레이어 또는 컴포넌트에 마우스를 올리고, 직접 연결된
  화살표와 반대편 컴포넌트를 시각적으로 구분한다.
- 변경 목표: 정적 전체 지도와 확대 경로를 보존하면서 pointer hover 동안만 연결선의 방향과
  범위를 짧은 모션으로 드러낸다.
- 비범위: SVG node·edge·label·좌표 변경, 상시 자동 재생, graph 편집·zoom 도구, 새로운 정보
  팝오버, landing 아키텍처 변경과 Multi Edge 기능 추가

## 2. 현재 상태 증거

기준 화면과 asset은 두 관련 리뷰의 1920·1280·390px light/dark 캡처 및 SHA-256을 재사용한다.
현재 두 panel은 SVG를 `<img>`로 표시하므로 page CSS와 JavaScript가 내부 Mermaid node·cluster·edge를
선택할 수 없다. 전체 canvas link의 hover·focus와 새 탭 확대만 가능하며, 특정 컴포넌트의 연결
관계는 사용자가 정적 선을 직접 따라가야 한다.

두 SVG에는 공통 Mermaid 식별 계약이 있다. 컴포넌트는 `my-svg-flowchart-<node>-<index>`, 레이어는
`my-svg-<layer>`, 연결선은 `data-id="L_<from>_<to>_<index>"`를 사용한다. 이 ID를 좌표와 별개인
상호작용 원본으로 사용할 수 있다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| Refero Tailscale style | `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 인프라 설명 media는 밝은 canvas, 제한된 accent와 명료한 경계를 유지 | 채택 — 기존 SVG 색과 shell 보존 |
| Refero PlanetScale style | `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 포커스가 필요한 기술 관계에만 interactive accent를 제한 | 채택 — 활성 연결선에만 teal 사용 |
| Refero Hashnode style | `001b4b2e-36c5-414f-943c-93047275fc18` | hover는 얇은 경계·surface 변화처럼 가볍고 기능적으로 표현 | 부분 채택 — 비관련 요소 감쇠, 장식 motion 기각 |
| Refero Relume sitemap | `9e35474d-09dd-4a7b-a108-a09aa37abf94` | 얇은 connector와 명확한 node hierarchy로 큰 구조의 관계를 추적 | 채택 — node와 직접 연결선 동시 강조 |
| Refero Resend screen | `c95dd62c-952d-4983-b0cb-dc4f049eb4f7` | 고빈도 hover는 약 80–120ms 상태 전환으로 즉시 반응 | 채택 — opacity 전환 120ms |
| Refero Design motion guide | 로컬 skill reference | 제품 hover는 짧은 feedback이어야 하며 reduced motion을 제공 | 채택 — 120ms 강조, 이동 모션 선택 해제 지원 |
| 현재 SVG 두 개 | `/assets/diagrams/*.svg` | node·cluster·edge의 결정적 ID와 arrow marker를 이미 보유 | 채택 — 새 좌표·그래프 라이브러리 불필요 |

2026-08-30에 위 Refero style 3개와 screen 2개를 다시 조회했다. 별도 flow 조사는 사용하지 않았다.
이번 변경은 여러 화면을 잇는 여정이 아니라 한 diagram 안의 순간적인 탐색 feedback이다.

## 4. Reference lock과 결정 원장

**Build target:** 현행 Starlight reader, 75rem figure, 두 TLS 탭과 원본 Mermaid SVG

**반드시 보존:** SVG 좌표·label·의미 색, 32px figure shell, 한 번에 panel 하나, 확대 link,
본문의 완결 설명, page-level 무가로 overflow, JavaScript 실패 시 정적 `<img>` fallback

| 결정 | 근거 | 역할 규칙 | 이유 |
|---|---|---|---|
| overview 두 panel만 progressive enhancement | 사용자 승인, 현행 정본 | docs interaction | landing과 standalone SVG의 정적 계약을 바꾸지 않는다. |
| same-origin SVG를 runtime inline으로 전환 | `<img>` DOM 경계 | interaction plumbing | 내부 node·cluster·edge를 선택할 수 있다. |
| Mermaid ID에서 edge 양 끝을 계산 | 현행 SVG 계약 | topology source | 좌표 기반 추정이나 수동 선 목록 drift를 피한다. |
| node hover는 incident edge만 강조 | Relume | component trace | 직접 연결 범위를 즉시 좁힌다. |
| layer hover는 소속 node의 내부·경계 edge 강조 | 사용자 요청 | layer trace | 레이어 책임과 외부 의존을 함께 읽는다. |
| 비관련 node·edge opacity 감쇠 | Tailscale, PlanetScale | hierarchy | 새 색을 늘리지 않고 현재 경로를 분리한다. |
| 120ms 상태 전환과 hover 중 dash flow | Resend, motion guide | feedback/direction | 즉시 반응하고 연결 방향을 기능적으로 설명한다. |
| reduced motion에서는 이동 제거 | motion guide | accessibility | 색·두께·opacity만으로 같은 선택 상태를 유지한다. |

### 채택

- SVG 파일은 그대로 두고 `<img>`가 먼저 표시되는 progressive enhancement를 사용한다.
- inline 시 두 SVG의 모든 ID와 내부 참조를 instance별 namespace로 바꿔 marker·filter 충돌을 막는다.
- 컴포넌트와 cluster는 pointer hit target으로 사용하되 새 keyboard tab stop 수십 개를 만들지 않는다.
  상호작용은 보조 시각화이며 동일 구조는 기존 본문과 canvas accessible name으로 제공한다.
- touch pointer에서는 hover 상태를 고정하지 않고 정적 지도와 확대 link를 유지한다.
- fetch·parse·계약 확인이 실패하면 오류 UI나 console noise 없이 원래 `<img>`를 보존한다.

### 기각

- SVG 안에 `<script>`나 event handler를 넣지 않는다. 정적 asset 안전 계약과 standalone 호환을
  유지한다.
- GSAP, graph library와 새 runtime dependency를 추가하지 않는다.
- 모든 edge의 상시 반복 animation, node 이동·scale, glow와 tooltip을 추가하지 않는다.
- hover만으로 새로운 설명 문구나 필수 상태를 노출하지 않는다.
- 생성 SVG를 손으로 편집하거나 좌표 기반 hit map을 별도 관리하지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | `<img>` 내부 선택 불가 | 두 overview panel이 성공 시 namespaced inline SVG로 전환되고 실패 시 원본 image가 남는다. |
| P0 | 컴포넌트 연결 추적 | node rollover에서 직접 incident edge와 양 끝 node만 강하게 보인다. |
| P0 | 레이어 연결 추적 | cluster rollover에서 소속 node의 내부·경계 edge가 모두 강조된다. |
| P1 | 방향 인지 | 기존 arrow marker를 보존하고 활성 path의 dash가 hover 동안만 흐른다. |
| P1 | 접근성·motion | reduced motion에서 dash animation 0, 정적 강조 유지, 새 keyboard trap과 필수 hover content 0이다. |
| P1 | 반응형 | 1920·1280·390px light/dark에서 크기·overflow·탭 전환이 기존 계약과 같다. |
| P1 | 안전·회귀 | 외부 SVG, script, event handler와 duplicate DOM ID가 없고 build gate·browser console 오류가 0이다. |

## 6. 구현 후 검증

### 6.1 구현 계약

- overview의 두 architecture link에만 progressive enhancement를 적용했다. 초기 선택 panel은 즉시
  inline SVG로 전환하고 숨겨진 NGINX panel은 탭 선택 시 처음 전환한다.
- SVG 안의 node·cluster·edge를 Mermaid 식별자로 연결하고, 두 instance의 DOM ID·marker·keyframe
  이름을 각각 namespace 처리했다.
- component rollover는 직접 연결 edge와 양 끝 node를, layer rollover는 소속 node의 내부·경계
  edge를 강조한다. 탭 전환·pointer 이탈 시 상태를 초기화한다.
- same-origin SVG와 `image/svg+xml`만 허용하고 `script`, `foreignObject`, event handler, 외부 URL 참조를
  거부한다. fetch·parse·topology 검증 실패 또는 JavaScript 비활성 환경에서는 기존 `<img>`가 남는다.
- opacity 전환은 120ms, 방향 dash는 560ms linear로 적용했다. `prefers-reduced-motion: reduce`에서는
  이동을 제거하고 색·3px 두께·opacity 강조는 유지한다.

### 6.2 로컬 브라우저 검증

Astro production preview와 headless Chromium으로 1920×1080, 1280×900, 390×844를 검사했다.

| 항목 | 결과 |
|---|---|
| direct topology | node 31, edge 40, layer 8, 중복 ID 0, unsafe element 0 |
| NGINX topology | node 32, edge 41, layer 9, 중복 ID 0 |
| component rollover | `workloadDomain` 직접 edge 4, 관련 node 5, 560ms dash offset 이동 확인 |
| layer rollover | `domains` member 6, 내부·경계 edge 25, 관련 node 16 |
| NGINX rollover | `nginx` 직접 edge 5, 관련 node 6 |
| 탭·lazy load | 숨겨진 NGINX는 최초 `<img>` 유지, 선택 후 inline, 이전 panel 강조 초기화 |
| reduced motion | animation `none`, 정적 3px 강조 유지 |
| dark theme | 활성선 `rgb(18, 98, 122)`, 밝은 SVG canvas에서 대비 유지 |
| 반응형·touch | 1280·390px 가로 overflow 0, SVG=canvas 폭, touch rollover 비활성 |
| 접근성·오류 | Axe WCAG A/AA 위반 0, console·page·request 오류 0 |
| JavaScript 비활성 | canvas 2, 원본 image 2, inline SVG 0, direct image 정상 로드 |

캡처는 활성 dash를 280ms 지점에 일시 정지해 결정적으로 기록했다.

| 캡처 | SHA-256 |
|---|---|
| `after-desktop-component.png` | `7fe5765bbbb5ece9cbc735bae2d8c2b9cb108857b59837c12ac2005ea9b9eaed` |
| `after-desktop-layer.png` | `5dfdf232d3990054e6f9963e55f869fb5d591076f0790ca650f2bfea0c84aa6d` |
| `after-desktop-nginx.png` | `b55358ab37371976f6b512ae69cd8c8530389f4d27418fc789143a36dc394775` |
| `after-desktop-reduced-motion.png` | `6556e37b455e0282a60ea47e86a920efe9d74bbb0809d23283acff30265ff52` |
| `after-desktop-dark-component.png` | `cbe8bc21119fa3f826f9decf1fb5e5e934addb96b423dd58281f578f2cfb64b2` |
| `after-mobile-static.png` | `a1e0e0432d8576b96337e990c26031d937e5368365e1db4613c8179fb2a2b4d5` |

### 6.3 저장소 게이트

- `npm run check`: PASS — Starlight 27 page build, Pages artifact 92 file 검증
- `make single`: PASS
- `make test`: PASS — g_test path 1,370개, audit startup 5/5
- `make check-all`: PASS — 공개 계약 gate 38개
- `make release`: PASS
- `scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS — source SHA-1
  `45519e8e`, cache `pcv-ui-v50c54abc`
- `python3 scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`: PASS
- `make check-public-comments`, `python3 scripts/strip_source_comments.py --check`: PASS — 공개 source
  comment 0
- interaction·site check script와 `ui/app.bundle.js`의 `node --check`: PASS
- `git diff --check`: PASS

원본 SVG asset은 수정하지 않았다. direct SVG SHA-256은
`f728f3460a50d44ccf388f3daf56d48882323b005de58838cbfa3385e52431b7`, NGINX 포함 full SVG는
`f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b`로 기존 checksum gate와 같다.

### 6.4 Pages 배포 검증

- source commit: `1e38eaf051508db41236921e4009b767438997bc`
- GitHub Actions: [`pages` run 33291113150](https://github.com/HardcoreMonk/purecvisor/actions/runs/33291113150)
  — build 21초, deploy 10초, 두 job 모두 PASS
- live 대상: `https://purecvisor.site/ko/getting-started/overview/#12-아키텍처-개요`

custom domain에서 로컬과 같은 Chromium 시나리오를 다시 실행했다. direct topology 31/40/8,
NGINX topology 32/41/9, 전체 duplicate ID 0을 확인했다. `workloadDomain`의 연결선 4개에서 120ms
사이 dash offset이 `5.7172` 이동했고, `domains` layer의 member 6·edge 25·node 16과 `nginx`의
edge 5·node 6이 일치했다. 1280·390px overflow 0, touch 정적 상태, reduced-motion animation
`none`, JavaScript 비활성 `<img>` 2개 fallback, Axe WCAG A/AA 위반 0, console·page·request 오류
0이었다.

| live 캡처 | SHA-256 |
|---|---|
| `after-desktop-component.png` | `18c786e9d988735de1f79e3d86dfd251e83a0e4efafa1fac6820c90da7daa8e2` |
| `after-desktop-layer.png` | `0e266b8d09f97fd7772d86130d32f4efbfdcdcf1444d92f17038ade4a01974f5` |
| `after-desktop-nginx.png` | `104602d2550ac16a98923938a4b131f8ee8f98513def5e841279a6f8476a7ca1` |
| `after-desktop-reduced-motion.png` | `6556e37b4555e0282a60ea47e86a920efe9d74bbb0809d23283acff30265ff52` |
| `after-desktop-dark-component.png` | `898dd8a1d96e0617c447a2a70f124dcded32fa322243cabdf59d053141472704` |
| `after-mobile-static.png` | `a1e0e0432d8576b96337e990c26031d937e5368365e1db4613c8179fb2a2b4d5` |
