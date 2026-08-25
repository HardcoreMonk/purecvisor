# 공개 랜딩 아키텍처 폭 맞춤·색상 의미 UI 리뷰

> **일자:** 2026-08-25
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 서비스 아키텍처
> **선행 리뷰:** `2026-08-25-landing-service-architecture-source-svg.md`

## 1. 목표와 제약

- SVG의 node·edge·label·좌표와 font를 유지한다.
- Clients, Config, API Transport, GMainLoop Control, Domain Modules, Persistent, Host의
  색상과 의미를 일대일로 연결한다.
- 제한 높이와 90rem 고정 폭 때문에 별도 canvas 안에서 마우스·키보드로 반복 scroll해야 했던
  구조를 제거한다.
- 기본 화면은 전체 흐름을 한 번에 보여 주고 세부 label은 같은 SVG의 새 탭 확대 보기로 제공한다.
- Single Edge 공개 범위, 한국어·영어, light·dark, keyboard focus와 색상 비의존 label을 유지한다.

## 2. Refero 연구와 reference lock

- Tailscale: 기술 문서용 흰 canvas, 얇은 경계, 제품 시각 자료를 설명 중심으로 다루는 방식
- Eraser: diagram 색을 넓은 장식면이 아니라 node·경계의 의미 부호로 제한하는 방식
- Timescale: 기술 도면을 전체 구조가 먼저 읽히는 설명 media로 다루는 방식
- Excalidraw: 전체 canvas와 zoom control을 분리해 overview와 detail inspection을 구분하는 방식

Reference lock:

- Primary: 사용자가 제공한 SVG의 구조·내용
- Preserve: viewBox, 192개 `<g>`, 51개 `<path>`, 62개 `<text>`, 210개 `<tspan>`, 모든 연결과 label
- Change only: SVG `<style>`의 layer palette와 cluster surface
- Page pattern: 전체 폭 fit, 내부 scroll 없음, 전체 SVG link와 별도 확대 action
- Reject: SVG 재배치, label 재작성, JavaScript pan/zoom, iframe/object viewer, 색상만 있는 범례

## 3. 결정 ledger

| 결정 | 근거 | 역할 | 이유 |
|---|---|---|---|
| `<style>` 제외 구조·내용 hash 고정 | 사용자 구조·내용 보존 요구 | integrity | 색상 변경이 node·edge·label 변경으로 번지는 것을 build에서 차단 |
| 7개 layer palette를 SVG 내부 style로 적용 | 사용자 색상 의미 체계 | diagram semantics | `<img>` 격리 문서에서도 원본 SVG가 같은 의미를 유지 |
| layer 이름과 의미를 함께 표시하는 범례 | DESIGN.md 색상 비의존 규칙 | accessibility | 색각과 theme에 관계없이 책임을 식별 |
| `width: 100%; height: auto` 전체 보기 | 사용자 scroll 문제 | overview | nested scroll을 없애고 페이지의 자연 scroll만 사용 |
| 다이어그램 전체를 확대 link로 사용 | Excalidraw overview/detail pattern | detail inspection | 별도 viewer runtime 없이 native SVG zoom 제공 |
| 밝은 중립 SVG canvas 유지 | SVG의 어두운 text와 투명 배경 | contrast | light·dark shell 양쪽에서 같은 판독 조건 유지 |

## 4. 색상 계약

| Layer | Fill | Stroke | Meaning |
|---|---|---|---|
| Clients | `#e7f3ff` | `#3a78b8` | 외부 진입점 |
| Config | `#ffebd8` | `#c45a0a` | 설정/비밀 |
| API Transport | `#f3edff` | `#7650a8` | 프로토콜 경계 |
| GMainLoop Control | `#e6f7ed` | `#27845d` | 핵심 제어 흐름 |
| Domain Modules | `#e5f7f6` | `#287f7a` | 비즈니스 로직 |
| Persistent | `#fff4e8` | `#b87543` | 상태 저장 |
| Host | `#f0f2f5` | `#667085` | 물리/커널 자원 |

## 5. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | SVG의 `<style>` 제외 구조·내용 hash가 제공 원본과 일치한다. |
| P0 | 세 landing에서 전체 SVG가 콘텐츠 폭에 맞춰 표시되고 내부 scroll container가 없다. |
| P0 | 7개 layer가 지정된 palette와 이름·의미 범례를 가진다. |
| P0 | 1440·1024·768·390px light·dark에서 page overflow, 겹침과 console·request error가 없다. |
| P1 | image와 별도 action이 같은 SVG를 새 탭에서 열고 keyboard focus가 보인다. |
| P1 | SVG safety, source map 금지, Single Edge 공개 경계와 reduced motion 계약을 유지한다. |

## 6. 구현 후 검증

- `cd site && npm run check`: PASS, 26 pages와 87 files artifact 검증
- 제공 원본과 배포 SVG의 `<style>` 제외 구조·내용 SHA-256 일치:
  `7cdc85160644201dae101319cdd968b8903298c8cd5a1868a01113df6ff0e5c6`
- 배포 SVG SHA-256: `1246431b9312d9e95e25cde517860e72ac3ad26255095df49c85fff3b141810c`
- 원본과 배포 SVG 모두 `<g>` 192개, `<path>` 51개, `<text>` 62개, `<tspan>` 210개 유지
- `xmllint --noout`: PASS, `<script>`·event handler·`<foreignObject>`·외부 link 없음
- 실브라우저 24개 조합: `/`, `/ko/`, `/en/` × light/dark × 1440/1024/768/390px
- canvas/image: 1440px `1198×1808`, 1024px `974×1470`, 768px `734×1108`,
  390px `356×537`로 모든 breakpoint에서 정확히 폭 맞춤
- 모든 조합에서 내부 scroll frame 0개, page overflow 0px, console·page·request error 0,
  Axe WCAG A/AA violation 0, 7개 색상·이름·의미 범례와 keyboard focus 확인
- light·dark desktop·mobile 캡처를 직접 비교해 layer palette, 밝은 SVG canvas, 전체 흐름과
  확대 action을 확인
- `make single`, `make test`: PASS, C23 Single Edge build와 1,370개 g_test·audit startup 통과
- `make check-all`: PASS, 38개 공개 계약 gate 통과
- `make release`: PASS, release build 경고 0
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS
- `python3 scripts/check_design_md.py`, `python3 scripts/strip_source_comments.py --check`,
  `make check-public-comments`, `git diff --check`: PASS

최종 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| light desktop | `e317b8994f7800bf5ad82e4b97d73ec3b5bb5da8acd5e89e24a1af0398c3033d` |
| dark desktop | `4e51ae3d52762d7cabf74bc1f9e0eb738e2aac3a7ece00ac7b0ebde4912a7c95` |
| light mobile | `f6b447078434d16aaee47531226e05fac4352f571e8f69b624a7defde4c18fb0` |
| dark mobile | `5b3f483a977038b7ef0d399ec9f211d6eaa404b9e6938509f82597eaa15b458f` |
