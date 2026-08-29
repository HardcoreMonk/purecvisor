# 공개 사이트 아키텍처 콘텐츠 현행화 UI 리뷰

> **일자:** 2026-08-30
> **판정:** PASS — 로컬 구현·구조·시각·접근성 검증 완료, 배포 미실행
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 서비스 아키텍처와 설명

## 1. 목표와 현재 상태

- 기존 공개 자산은 Monitoring Source v2, `pcv_monitoring.db`, BPF LSM audit 경계를 담지
  못한 2026-08-25 구조였다.
- header, hero, CTA, 7계층 범례, 전체 폭 figure와 light/dark·responsive 동작은 변경 대상이
  아니다.
- 현재 라이브 기준은 `https://purecvisor.site/`이며 교체 전 SVG SHA-256은
  `1246431b9312d9e95e25cde517860e72ac3ad26255095df49c85fff3b141810c`다.

## 2. 연구와 reference lock

- 1차 lock: `2026-08-25-landing-architecture-fit-semantic-colors.md`의 Tailscale·Eraser·
  Timescale·Excalidraw 합성과 7계층 palette, 폭 맞춤, native SVG 확대 경로
- 보조 lock: 제품 저장소의 `2026-08-28-docs-site-parity-and-source-update.md`에서 승인한
  white fixed header, 단일 teal accent, 7계층 legend와 현재 아키텍처 정본 우선 원칙
- 렌더 안전 근거: Mermaid 공식 설정의 root `htmlLabels=false`와
  `markdownAutoWrap=false`를 사용해 label을 SVG `<text>/<tspan>`으로 생성했다.

이번 변경은 시각·정보 구조를 새로 설계하지 않고 승인된 구조 안의 내용만 현행화한다. 따라서
새 Refero 평균값을 만들지 않고 선행 reference lock을 그대로 승계한다.

## 3. 결정

| 결정 | 근거 | 판정 |
|---|---|---|
| 6개 서비스 도메인과 Monitoring Source v2 영속 상태를 포함 | 현재 Single Edge 아키텍처 정본 | 채택 |
| DPDK와 BPF LSM을 host 조건부·audit-only로 명시 | 기능 가용성과 enforcement 과장 방지 | 채택 |
| 기존 7계층 palette·legend·layout 보존 | 선행 UI 리뷰와 사용자의 현재 탐색 흐름 | 채택 |
| 최신 Mermaid의 `<foreignObject>` 산출물 직접 배포 | 공개 SVG 안전 계약 위반 | 기각 |
| 기능 카드나 별도 상태 섹션 추가 | landing 정보 구조 변경과 중복 | 기각 |

## 4. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | SVG에 Monitoring Source v2, `pcv_monitoring.db`, DPDK, BPF LSM 경계가 존재한다. |
| P0 | `<script>`, `<foreignObject>`, event handler와 외부 link가 0건이다. |
| P0 | `/`, `/ko/`, `/en/`이 같은 자산·intrinsic ratio를 사용하고 build hash gate가 고정된다. |
| P1 | 한국어·영어 설명이 DPDK·LSM의 호스트 전제와 audit-only 제한을 과장 없이 전달한다. |
| P1 | light/dark desktop/mobile에서 clipping·가로 overflow·접근성 회귀가 없다. |

## 5. 구현 후 검증

- `cd site && npm run check`: PASS, 26개 page와 87개 artifact 검증
- SVG: 121,655바이트, `<g>` 186개, `<path>` 48개, `<text>` 60개, `<tspan>`
  326개. `<script>`·`<foreignObject>`·event handler·외부 link는 0개이며 `xmllint`를 통과했다.
- 배포 파일 SHA-256:
  `f8b155814b517425827570309782d43b4cf840140b20f6d9f1d3e404abc9f906`
- `<style>` 제외 구조·내용 SHA-256:
  `ad0ca4159c538fd7eb96ae8250a887ea5d1188faa3cef0bf194e2191675cf5db`
- `/`, `/ko/`, `/en/` × 1440·390px × light·dark 12개 조합에서 console·page·request
  error 0, 가로 overflow 0, Axe WCAG A/AA violation 0을 확인했다.
- image/figure: desktop `1198×1554`/`1200px`, mobile `356×462`/`358px`로
  intrinsic ratio와 figure 폭이 일치했다. 설명 문구는 12개 조합 모두 표시됐다.
- 시각 검토: white SVG canvas, 7계층 범례와 기존 header·hero·CTA 계층을 보존했고
  Monitoring Source와 영속 상태가 추가돼도 label clipping·겹침이 없다.

최종 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| light desktop | `402121ae1b622f4ec8326010a24e36a8472c03bc6d74015b4e3c3d374b43fee2` |
| dark desktop | `e53c21622cac5b6100dee31f5e9a1c8eeea1a68901dda921741570f1b3fd3ae4` |
| light mobile | `a60a12fd43598b794d91f7aed776b63e2a62b09c506d1833c847fd61c6ce16a6` |
| dark mobile | `fa40a1369b4bb03c6b6ef88f167545fbd04d2f63c02a324d59e8c344cd5c0ff6` |

이 PASS는 로컬 산출물 판정이다. `purecvisor.site` live 반영은 public repository의
commit·push와 Pages workflow 성공 뒤 별도로 확인한다.
