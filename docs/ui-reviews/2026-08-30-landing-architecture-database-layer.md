# 공개 랜딩 서비스 아키텍처 DB 계층 보강 UI 리뷰

> **일자:** 2026-08-30
> **판정:** LIVE-PASS — 로컬 구현·구조·시각·접근성 및 GitHub Pages·custom domain 검증 완료
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 서비스 아키텍처 SVG

## 1. 목표와 확인된 문제

- 기존 SVG는 영속 상태를 범용 `SQLite WAL` 노드와 `pcv_monitoring.db` 노드로 축약해
  데이터베이스 파일 전체를 식별할 수 없었다.
- 실제 Single Edge 정본은 로컬 SQLite 파일 10개를 사용하며 기존 SVG에는
  `pcv_webpush.db`가 표시되지 않았다.
- 운영자와 개발자는 전체 서비스 흐름을 읽는 자리에서 DB 책임과 실제 파일명을 확인할 수
  있어야 한다.

## 2. 연구와 reference lock

- 1차 lock은 `2026-08-25-landing-architecture-fit-semantic-colors.md`의 Tailscale·Eraser·
  Timescale·Excalidraw 합성, 7계층 palette, 전체 폭 SVG와 native 확대 경로다.
- 보조 lock은 `2026-08-30-public-site-current-state-refresh.md`의 white canvas, 기존
  header·hero·CTA·범례와 `htmlLabels=false`, `markdownAutoWrap=false` 렌더 안전 계약이다.
- 이번 작업은 승인된 아키텍처 figure 안의 영속 계층 내용 보강이다. 새 page pattern이나
  visual token을 도입하지 않으므로 새 Refero 조사를 평균화하지 않고 선행 lock을 승계한다.

## 3. 결정 ledger

| 결정 | 근거 | 판정 |
|---|---|---|
| DB를 `Core / identity / security / network` 7개와 `Operations` 3개 노드로 표시 | 데이터베이스 아키텍처 정본 | 채택 |
| 두 노드 안에 실제 파일명 10개를 모두 표기 | 사용자가 지적한 식별성 누락 | 채택 |
| SQLite 밖 desired state는 별도 노드로 유지 | DB 복원과 전체 서비스 상태 복원을 혼동하지 않기 위함 | 채택 |
| 7계층 palette와 기존 landing 정보 구조 유지 | 선행 UI 리뷰 reference lock | 채택 |
| DB 10개를 각각 독립 노드로 배치 | 전체 폭 증가와 mobile label 축소 | 기각 |
| DB 설명 카드나 별도 landing section 추가 | Hero 정보 구조 중복 | 기각 |

## 4. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | SVG에 10개 DB 파일명이 각각 정확히 한 번 존재한다. |
| P0 | 기존 범용 SQLite 노드와 단독 Monitoring DB 노드가 두 책임별 DB 노드로 대체된다. |
| P0 | `<script>`, `<foreignObject>`, event handler와 외부 link가 0건이다. |
| P0 | `/`, `/ko/`, `/en/`이 같은 SVG와 새 intrinsic ratio를 사용한다. |
| P1 | 7계층 palette, white canvas, 전체 폭 표시와 native 확대 동작이 유지된다. |
| P1 | desktop/mobile, light/dark에서 clipping·가로 overflow·접근성 회귀가 없다. |

## 5. 구현 후 검증

- 정본 Mermaid를 `htmlLabels=false`, `markdownAutoWrap=false`, ELK 설정으로 렌더하고 기존
  semantic palette를 적용했다.
- SVG는 124,089바이트, `1849.5234375×2798` viewBox, `<g>` 186개, `<path>` 48개,
  `<text>` 60개, `<tspan>` 341개다.
- 배포 파일 SHA-256은
  `f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b`, `<style>` 제외
  구조·내용 SHA-256은
  `0f3f3a26d1dc2b128a0b58da6f63bad61d71637e6a3d4aa2f01aff9f137778be`다.
- 10개 DB 파일명이 각각 1회 존재하고 이전 `SQLite WAL: VM state`·`monitorStore`는 0건이다.
  `<script>`·`<foreignObject>`·event handler·외부 link도 모두 0건이며 `xmllint`를 통과했다.
- `cd site && npm run check`: PASS, 27개 page와 90개 artifact 검증.
- `/`, `/ko/`, `/en/` × 1440·390px × light·dark 12개 조합에서 console·page·request
  error 0, 가로 overflow 0, Axe WCAG A/AA violation 0을 확인했다.
- image/figure는 desktop `1198×1812`/`1200px`, mobile `356×539`/`358px`로 새 intrinsic
  ratio를 유지했다. 10개 DB를 알리는 note·alt도 12개 조합 모두 표시됐다.
- 시각 검토에서 두 DB 노드의 파일명 겹침·clipping이 없고 7계층 palette, white canvas,
  기존 header·hero·CTA·범례와 native 확대 action이 유지됨을 확인했다.
- `make single`, `make test` 1,370개와 audit startup 5개, `make check-all` 38개 게이트,
  `make release`, UI bundle freshness, DESIGN 계약과 공개 source comment 0건을 통과했다.

대표 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| light desktop | `cf87e58086c1916f377b9a5f84df0166457f49c863b57b234ca4498c56883bfd` |
| dark desktop | `9549b9aea9fca2a759724f046981ac9510d9e389b126f4449495f791ce040edf` |
| light mobile | `2cd5bf5b311ad318d930781cac900942761081844ed1852846276599a71b4e04` |
| dark mobile | `4b8d2341e450d613eb0f2cf9d321628b681471ff5acd7135a6994b4c59295b07` |

정본 commit `38088967`과 public 콘텐츠 commit `3baad6c`를 각각 `main`에 push했다.
GitHub Pages run `33278756764`의 build·deploy가 모두 성공했고 `https://purecvisor.site/`,
`/ko/`, `/en/`과 SVG는 HTTP 200을 반환했다. live SVG는 로컬 정본과 바이트 단위로 같으며
SHA-256도 `f64b3756dbe546ac65245fa5363d61cbd30e03b1652b53c612ca72e33d685c3b`로 일치했다.
live HTML은 새 `1849.5234375×2798` intrinsic size와 한국어·영어 10개 DB 설명을 포함한다.
따라서 이 리뷰는 로컬 PASS에서 **LIVE-PASS**로 승격한다.
