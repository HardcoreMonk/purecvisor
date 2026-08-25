# 공개 랜딩 서비스 아키텍처 SVG 원본 교체 UI 리뷰

> **일자:** 2026-08-25
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 서비스 아키텍처
> **승계:** 같은 날 색상 의미 체계와 폭 맞춤 전체 보기 적용 이후 현재 구현 기준은
> `2026-08-25-landing-architecture-fit-semantic-colors.md`가 소유한다.
> **대체:** `2026-08-24-landing-service-architecture-completion-domains.md`의 code-native figure

## 1. 요청과 입력 자료

사용자는 압축 해제 자료의 서비스 아키텍처 다이어그램을 재구성하지 않고 SVG 원본 그 자체로
교체하도록 명시했다. 전체·제어면·도메인 3개 SVG 가운데 landing의 전체 서비스 아키텍처에
대응하는 `purecvisor-single-full-architecture.svg`를 적용 대상으로 확정했다.

| 항목 | 확인 결과 |
|---|---|
| 원본 크기 | `1885.3125×2845.599853515625` |
| 원본 용량 | 110,764바이트 |
| SHA-256 | `0890224b4854f36dfb9b7dc6ae4be78b855fa9623a97d7ba2fbffb1edf7d9ca1` |
| SVG text | `<text>` 62개, `<tspan>` 210개, `htmlLabels: false` 결과 |
| 보안 표면 | `<script>`, event handler, `<foreignObject>`, link와 data URI 없음 |
| 공개 범위 | Single Edge 런타임·도메인·영속 상태·Linux/KVM host, Multi Edge 기능 없음 |

## 2. 연구와 reference lock

이번 교체의 primary visual source는 사용자가 제공한 전체 SVG 원본이다. 선행 Refero 연구의
Tailscale infrastructure precision, PlanetScale의 절제된 canvas, Relume의 diagram hierarchy는
SVG를 둘러싼 page shell과 scroll viewport에만 제한해 사용한다. SVG 내부의 색, font, node,
connector와 좌표는 변경하지 않는다.

Reference lock:

- Primary: 제공된 `purecvisor-single-full-architecture.svg`
- Preserve: 파일 바이트, intrinsic size, viewBox, 모든 node·edge·label과 Mermaid render style
- Borrow only: 기존 landing의 32px figure shell, 얇은 border, compact source action
- Media strategy: same-origin 정적 SVG를 `<img>`로 직접 렌더하고 원본 link도 같은 파일을 가리킴
- Reject: inline 재작성, SVG 최적화, font·palette override, PNG fallback, domain 선택 재구성

## 3. 결정 ledger

| 결정 | 근거 | 역할 | 이유 |
|---|---|---|---|
| 전체 SVG 한 파일을 배포 자산으로 사용 | 사용자 명시 요청 | primary media | 재해석 없이 원본 구조를 그대로 제공 |
| SHA-256을 build gate로 고정 | 원본 보존 요구 | integrity | formatter·optimizer에 의한 무의식적 변경 차단 |
| 90rem 표시 폭과 양방향 scroll | 원본 1885px 폭·긴 세로 비율 | readability | mobile에서 전체 폭 축소로 label이 소실되는 문제 방지 |
| 별도 새 탭 원본 link | SVG native zoom | secondary action | browser zoom과 전체 canvas 열람 경로 제공 |
| 밝은 고정 canvas | 투명 SVG와 원본 dark text | media surface | dark theme에서도 원본 대비를 변경 없이 유지 |
| alt와 focus 가능한 region | `<img>` 외부 SVG | accessibility | 내부 SVG document에 의존하지 않고 구조와 조작 방법 전달 |
| 기존 path selector와 JS 제거 | 원본 직접 사용 | runtime | 원본과 별도 재구성 상태가 동시에 존재하는 혼선 제거 |

## 4. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 세 landing이 같은 `/assets/diagrams/purecvisor-single-full-architecture.svg`를 한 번씩 직접 사용한다. |
| P0 | 배포 SVG SHA-256과 intrinsic size가 제공된 원본과 일치한다. |
| P0 | 기존 `.pcv-arch-*`, `data-active-path`, domain selection script가 landing과 CSS에서 제거된다. |
| P0 | 1440·1024·768·390px의 light·dark에서 page overflow와 console·request error가 없다. |
| P1 | scroll viewport가 keyboard focus와 touch pan을 지원하고 source link가 같은 SVG를 연다. |
| P1 | 한국어·영어 alt와 scroll region accessible name이 제공된다. |
| P1 | reduced motion, source map 금지와 Single Edge 공개 경계가 유지된다. |

## 5. 구현 후 검증

- `cd site && npm run check`: PASS, 26 pages와 87 files artifact 검증
- 입력·public·dist SVG SHA-256 일치:
  `0890224b4854f36dfb9b7dc6ae4be78b855fa9623a97d7ba2fbffb1edf7d9ca1`
- 실브라우저 24개 조합: `/`, `/ko/`, `/en/` × light/dark × 1440/1024/768/390px
- Axe WCAG A/AA violation 0, console·page·request error 0, page overflow 최대 0px
- SVG natural size `1885×2846`, rendered size `1440×2173`, figure radius 32px과 밝은 중립
  viewport·image 배경 유지
- viewport client/scroll: 1440px `1168×792 / 1440×2173`, 1024px `944×648 /
  1440×2173`, 768px `704×612 / 1440×2173`, 390px `326×549 / 1440×2173`
- 모든 조합에서 양방향 scroll, keyboard focus, 언어별 region name, 동일 SVG source link와
  기존 `.pcv-arch-*` node 0개 확인
- `make single`, `make test`: PASS, C23 Single Edge build와 1,370개 g_test·audit startup 통과
- `make check-all`: PASS, 38개 공개 계약 gate 통과
- `make release`: PASS, release build 경고 0
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS
- `python3 scripts/check_design_md.py`, `python3 scripts/strip_source_comments.py --check`,
  `make check-public-comments`, `git diff --check`: PASS
- light·dark desktop·mobile 캡처를 직접 비교해 SVG 색·한국어 label·투명 여백 배경·shell clipping과
  안내 문구를 확인

최종 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| Korean light desktop | `e6fc6f0ba670c69d7368789796fbb5f060268fd215f923afe722fcd361003a99` |
| Korean dark desktop | `9b359691481bcd1c9a838be6d0227a9e50b4b06c2ae7568a85c6103334b336b9` |
| Korean light mobile | `6c5303882edc8d447c4d13a283ad504e61754c0cd9b2b12e4cee6287454301e0` |
| Korean dark mobile | `a1e55282e99634bb0b51fd7c6496533261aa98282dcf8c4c5e438d5b28e31842` |

P0·P1 수용 기준을 모두 충족했으며 열린 항목은 없다.
