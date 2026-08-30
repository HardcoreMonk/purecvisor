# 랜딩 아키텍처 SVG 노출 제거 운영 인계

> **일자:** 2026-08-30
> **상태:** LIVE-PASS
> **대상:** `/`, `/ko/`, `/en/`

## 반영 내용

- 한국어 root·`/ko/`와 영어 `/en/` landing에서 전체 architecture figure, 7개 layer 범례,
  SVG image, 확대 link와 장문 note를 제거했다.
- landing에서만 사용하던 layer legend CSS token·selector·breakpoint rule을 제거했다.
- eyebrow, H1, lead, 5분 퀵스타트, 전체 운영 가이드와 배포 note는 변경하지 않았다.
- 상세 architecture SVG와 direct/NGINX mode tab·rollover interaction은 시작하기 overview에 유지했다.

## 변경·배포 기준

| 항목 | 값 |
|---|---|
| 기능 commit | `4330260492463f1ac823ac2469b35dbd92158e23` |
| 직전 public commit | `4ec14ff` |
| Pages run | `33302637830` |
| build / deploy | PASS, 24초 / 9초 |
| live route | `https://purecvisor.site/`, `/ko/`, `/en/` |
| 상세 overview | `/ko/getting-started/overview/#12-아키텍처-개요` |

## 검증 결과

- live 18개 조합: 3 route × 1920·1280·390px × light/dark
- architecture figure·legend·image·link와 `/assets/diagrams/` request: 전부 0
- Hero 높이: desktop `515.6875px`, 1280px `502.515625px`, mobile `514.984375px`
- CTA: route별 2개, target 48px
- 가로 overflow, Axe WCAG A/AA 위반, console·page·request 오류: 전부 0
- local/live 대표 캡처 8개: SHA-256 pixel-identical
- overview direct topology 31/40/8·rollover edge 4, NGINX topology 32/41/9 유지
- 저장소 검증: Pages 27 page·92 artifact, C test 1,370개, audit startup 5/5,
  공개 계약 gate 38개와 release build 모두 PASS

상세 Refero 근거, 변경 전후 실측과 캡처 hash는
[UI 리뷰](../ui-reviews/2026-08-30-landing-architecture-removal.md)에 기록했다.

## 운영·롤백

- landing에서 SVG가 다시 보이거나 diagram request가 발생하면 `site/src/content/docs/index.mdx`,
  `site/src/content/docs/en/index.mdx`와 `site/scripts/check-site.mjs`의 retired marker gate를 확인한다.
- 상세 architecture 접근은 landing이 아니라 시작하기 overview에서 제공하는 것이 현재 정본이다.
- 변경만 롤백해야 하면 `4ec14ff`의 landing figure·legend markup과 CSS를 복원한 뒤 Pages workflow와
  세 route의 SVG request·overflow·Axe를 다시 확인한다.
- SVG asset, 서버 네트워크, 데이터베이스, `purecvisorsd`와 NGINX 운영 설정은 변경하지 않았다.
