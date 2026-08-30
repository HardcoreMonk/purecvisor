# 시작하기 아키텍처 TLS 모드 탭 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LOCAL-PASS — 구현·로컬 검증 완료, GitHub Pages 검증 대기
> **승인:** 2026-08-30 사용자 명시 요청
> **대상:** `/ko/getting-started/overview/#12-아키텍처-개요`
> **관련 리뷰:** `2026-08-30-overview-architecture-completeness.md`,
> `2026-08-30-public-documentation-reading-axis.md`

## 1. 제품 맥락과 목표

- 사용자: Single Edge 설치 모드에 맞는 실제 외부 요청 경계를 확인하려는 운영자와 개발자
- 핵심 작업: `purecvisorsd` 직접 HTTPS와 선택형 NGINX 외부 TLS 종료 중 하나를 선택해,
  해당 모드의 전체 아키텍처만 읽고 원본 SVG를 확대한다.
- 변경 목표: NGINX가 포함된 단일 그림만 제공하던 현행 상태를 두 개의 독립 SVG와 접근 가능한
  탭으로 분리하고, 제품 기본값인 `purecvisorsd` 직접 HTTPS를 첫 상태로 제공한다.
- 비범위: TLS 이외의 제어면·서비스 도메인·DB·호스트 구조 변경, 새로운 색상 체계, landing
  아키텍처 교체, Multi Edge 기능 추가

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-30 KST
- 라이브 URL: `https://purecvisor.site/ko/getting-started/overview/#12-아키텍처-개요`
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-30-overview-architecture-completeness/live-1920-light-figure.png`
- desktop SHA-256: `2d40436b02cd127619c09fa359796d9d94fa34d5acc1eb87763526d90f2b1297`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-30-overview-architecture-completeness/live-390-light-figure.png`
- mobile SHA-256: `5b55ec6e4bed411b56c42869b327be2287193eabf41e714a3d9f7c9848da4b9d`

현행 figure는 `purecvisor-single-full-architecture.svg` 하나만 표시한다. 본문과 caption은 NGINX가
선택형이며 daemon 직접 HTTPS가 기본이라고 설명하지만, 시각 자료에는 NGINX 경로만 있어 기본
설치의 실제 ingress를 한눈에 확인할 수 없다. ADR-0029는 daemon 자체 TLS를 기본으로, loopback
신뢰 경계가 성립하는 호스트의 NGINX 외부 종료만 명시적 opt-in으로 정의한다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| Refero Tailscale style | `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 밝은 중립 canvas, 얇은 경계와 제한된 accent가 인프라 자료의 계층을 유지 | 채택 — 기존 PureCVisor shell과 token 유지 |
| Refero PlanetScale style | `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 기술 자료 switcher는 장식보다 선명한 active/inactive 상태와 넓은 구조 폭을 우선 | 부분 채택 — sharp한 상태 구분만 차용, 고유 서체·색은 기각 |
| Refero Hashnode style | `001b4b2e-36c5-414f-943c-93047275fc18` | 단일 saturated accent를 interactive/active 상태에만 제한 | 채택 — 기존 teal을 선택·focus 역할에만 사용 |
| Refero Relume sitemap screen | `9e35474d-09dd-4a7b-a108-a09aa37abf94` | 같은 diagram canvas 위에서 두 view mode를 인접 탭으로 즉시 전환 | 채택 — figure 바로 위 2개 모드 탭 |
| Refero Resend API screen | `c95dd62c-952d-4983-b0cb-dc4f049eb4f7` | 짧은 탭 label, 명시적 선택 상태와 선택한 기술 자료만 노출 | 채택 — 한 번에 SVG 하나만 표시 |
| ADR-0029 | `docs/adr/0029-rest-ws-tls-always-on.md` | daemon 자체 HTTPS가 기본이고 NGINX 외부 종료는 loopback 제한 opt-in | 채택 — 기본 탭과 두 SVG의 ingress 계약 |

별도 flow 연구는 사용하지 않았다. 이 변경은 시작·완료 단계가 있는 여정이 아니라 같은 위치에서
두 정적 자료 중 하나를 선택하는 단일 상태 전환이다.

## 4. Reference lock과 결정 원장

**Build target:** 현행 Starlight reader, 75rem figure와 전체 아키텍처 SVG의 계층·색상·밀도

**변경하면 안 되는 항목:** 흰/soft-gray canvas, Pretendard 기반 본문, teal의 interactive 역할,
32px figure shell, 7개 의미 계층, 제어면 이후 node·edge·label, mobile page-level 무가로 overflow

| 결정 | 근거 | 역할 규칙 | 이유 |
|---|---|---|---|
| 직접 HTTPS를 기본 선택 | 사용자 요청, ADR-0029 | 제품 기본값 | 첫 화면과 기본 배포를 일치시킨다. |
| figure 위 2개 horizontal tab | Relume screen | mode switch | 다이어그램과 제어의 관계가 즉시 보인다. |
| 선택 panel 하나만 표시 | Resend screen | content replacement | 두 장의 긴 SVG를 동시에 쌓지 않는다. |
| active border·soft surface·teal text | PlanetScale, Hashnode, `DESIGN.md` | selected/interactive | 색상 외에도 border와 surface로 선택을 전달한다. |
| SVG별 확대 link·caption | 현행 figure, 사용자 요청 | media action | 선택한 파일과 설명의 대응을 유지한다. |
| 전환 animation 없음 | 사용자 자료 규모, visual QA 규칙 | layout stability | 큰 종횡비 차이에 따른 흔들림을 피한다. |

### 채택

- WAI-ARIA tab 구조와 roving `tabindex`를 사용하고 좌우 방향키, Home, End를 지원한다.
- tab label은 `purecvisorsd 직접 HTTPS`, `NGINX 외부 TLS 종료`로 기능을 직접 명명한다.
- 새 Mermaid 원본과 SVG는 직접 HTTPS 진입만 다르고 daemon 내부부터 host까지 기존 구조를 유지한다.
- 각 SVG의 intrinsic width·height, 구체적 alt와 새 탭 확대 link를 제공한다.
- mobile에서는 tab이 가용 폭 안에서 2열을 유지하되 label wrap과 40px 이상 target을 허용한다.

### 기각

- select/dropdown으로 모드를 숨기지 않는다. 두 선택지를 동시에 비교할 수 있어야 한다.
- 두 SVG를 세로로 모두 표시하지 않는다. 문서 길이와 중복을 과도하게 늘린다.
- NGINX를 기본 tab으로 두거나 기본 daemon 그림에 NGINX를 흐리게 남기지 않는다.
- 외부 레퍼런스의 blue/red/orange accent, monospace 전면 적용과 pill 장식을 복제하지 않는다.
- tab 전환을 자동 재생하거나 선택 시 URL hash를 바꾸지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 기본 설치 그림 부재 | 별도 직접 HTTPS SVG에 NGINX node가 없고 Web UI·REST·metrics가 daemon transport로 직접 연결된다. |
| P0 | 모드 선택 | 기본 탭은 직접 HTTPS이며 선택한 panel·caption·확대 link만 표시된다. |
| P0 | 구조 drift | transport 이후 제어면, 6개 domain, SQLite DB 10개, desired state와 host 계층은 두 SVG에서 동일하다. |
| P1 | keyboard 접근성 | Tab으로 진입하고 좌우 방향키·Home·End로 탭과 panel을 전환하며 focus가 보인다. |
| P1 | 반응형 | 1920·1280·390px light/dark에서 page overflow, 겹침과 잘린 tab label이 없다. |
| P1 | 정적 계약 | 두 asset의 hash·필수 marker·unsafe SVG 부재와 tab ARIA 구조를 build gate가 검사한다. |
| P1 | 브라우저 품질 | console/page/request error와 Axe WCAG A/AA 위반이 0이다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: native `button[role=tab]`, roving `tabindex`, `aria-selected`, `aria-controls`,
  `tabpanel[aria-labelledby]`을 연결하고 `:focus-visible` ring을 유지한다.
- 색상 외 상태 표현: 선택 탭은 accent text뿐 아니라 하단 border와 배경 surface가 함께 바뀐다.
- loading/empty/error/disabled: same-origin 정적 SVG라 비동기 loading UI는 만들지 않는다. asset 부재와
  변조는 build gate에서 실패하고, JS가 실행되지 않아도 기본 직접 HTTPS panel은 읽을 수 있다.
- 1024/768/480px: figure는 기존처럼 폭에 맞춰 축소한다. tab target은 40px 이상이고 두 label은
  필요 시 내부 줄바꿈하며 page-level 가로 scroll을 만들지 않는다.

## 7. 정량 검증

- Attention Insight: 사용하지 않음. 핵심 문제는 시선 분포가 아니라 모드별 ingress의 정확성과
  선택 상태·keyboard 계약이다.

## 8. 구현 후 검증

- 구현: 별도 Mermaid 원본과 직접 HTTPS SVG를 추가하고, 시작하기 `1.2`에 직접 HTTPS 기본
  선택·NGINX 선택형의 두 `tab`/`tabpanel`을 연결했다. 탭은 click, 좌우 방향키, Home, End와
  roving `tabindex`를 지원한다.
- 정적·회귀 검사: `npm run check`, `node --check site/scripts/check-site.mjs`,
  `node --check site/scripts/update-architecture-colors.mjs`, `git diff --check`,
  `python3 scripts/check_design_md.py`, `tests/integration/test_design_md_surface.sh`,
  `make check-public-comments`, `python3 scripts/strip_source_comments.py --check`, `make single`,
  `make test`(g_test 1,370개와 audit startup 5개), `make check-all`(계약 게이트 38개),
  `make release`, `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`,
  `python3 scripts/check_ui_bundle_fresh.py`, `node --check ui/app.bundle.js`가 모두 통과했다.
- 실브라우저: Chromium에서 light/dark 각각 1920×1080, 1280×900, 390×844를 검사했다. 두
  asset은 `200 image/svg+xml`, page-level overflow·console/page/request error·Axe WCAG A/AA
  위반은 모두 0이다. click과 좌우 방향키·Home·End 전환, 선택 탭 focus, 40px 이상 target도
  통과했다.
- 1920px 캡처 SHA-256: light 직접
  `82dd5b4b62f79e350f83107bd640e472b62da1dea996c1f55ab704cce2d621f0`, light NGINX
  `7b591a2ab537269bb817eab8bd2567c74b7f013cae6717697ab590eff670bc00`, dark 직접
  `665a9178e48724c4c1def0d78155cb00b5288799761dc889951ee94fa786d474`, dark NGINX
  `97a39188abfd2d0509b47a05e55e9863a602a93d9c0924f4555faee3fbb86826`.
- 390px 캡처 SHA-256: light 직접
  `d87ebd6f8f1660ca842edb6b08c90050935cf9c72e726db870d426793ccce069`, light NGINX
  `09a3615d47b165f1ab1cfc6580d9a6b1bca97066c61ee0e50f8e2b89526464f1`, dark 직접
  `087edbdbb1eb32e2e82c0bdfdbceb790fc84fc6e93497cdf85de56fe715b6ce8`, dark NGINX
  `741a7d7f75bda8e841d4b695259db7e712cb4186c2b0f4e220ef05b7b16fd3fe`.
- 1280px에서도 동일 항목을 통과했다. 직접 HTTPS SVG 배포 파일 SHA-256은
  `f728f3460a50d44ccf388f3daf56d48882323b005de58838cbfa3385e52431b7`, style을 제외한
  구조·내용 SHA-256은 `caa00de89a242a2060ca56753e51b0348a643643195759c568ad2e243f8de6dd`다.
- 잔여 위험: SVG 두 파일의 공통 구조가 이후 변경에서 함께 갱신되어야 한다.
