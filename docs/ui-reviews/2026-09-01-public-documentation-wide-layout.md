# 공개 문서 넓은 화면 공간 활용 UI 리뷰

> **일자:** 2026-09-01
> **판정:** LIVE-PASS — 로컬·GitHub Pages·custom domain 시각·접근성 검증 완료
> **요청:** GitHub Pages 본문의 좌·우 잔여 공간을 최대한 활용
> **대상:** Starlight reader의 좌측 탐색, 본문, 기술 자료, 아키텍처 자료와 우측 목차
> **관련 결정:** ADR-0047
> **대체 대상:** `2026-08-30-public-documentation-reading-axis.md`의 폭 상한

## 1. 설계 brief

1920px 이상의 화면에서 좌·우 navigation과 문서 본문 사이의 불필요한 여백을 줄이고,
표·code·SVG가 가용 공간을 적극적으로 사용하게 한다. 일반 문장은 화면 전체 폭으로 늘리지
않고 판독 가능한 상한을 유지한다. 기존 Pretendard, 흰 canvas, restrained teal, 공통 좌측
읽기 축, 표 내부 scroll과 모바일 단일 열 동작은 보존한다.

## 2. 변경 전 실측

1920px 데이터베이스 아키텍처 page의 실제 browser geometry는 다음과 같았다.

| 영역 | 변경 전 |
|---|---:|
| 좌측 탐색 | 304px |
| 우측 목차 rail | 360px |
| Markdown canvas | 1,200px |
| 일반 본문·H1·pagination | 736px |
| 표·code 최대 폭 | 960px |
| 아키텍처 figure | 1,200px |

일반 본문은 `46rem`, 기술 자료는 `60rem`, 아키텍처 canvas는 `75rem`, Starlight sidebar는
`19rem`이었다. 1280px에서는 좌·우 rail을 제외한 본문 가용 폭이 624px까지 줄었다. 네트워크
page는 추가로 prose를 `43rem`으로 축소해 같은 shell 안에서도 더 큰 오른쪽 공백을 만들었다.

## 3. 연구 근거와 reference lock

| 근거 | 확인한 패턴 | 적용 |
|---|---|---|
| Refero Tailscale style `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 절제된 기술 문서 표면, 구조화된 넓은 container, 선명한 정보 계층 | PureCVisor 색·서체·surface는 변경하지 않음 |
| Refero Make Help screen `25896602-e5bf-4bf2-8765-9cf6355af6fe` | 약 240px 탐색·가변 본문·약 220px 목차의 3열 reader | 양쪽 rail을 줄이고 중앙 가용 폭 확대 |
| Refero Cursor Docs screen `9e72cc04-43e8-485b-965d-214c43bcf7ba` | 220~260px 탐색과 약 760~900px의 유동 본문 | 일반 본문 상한을 896px로 제한 |
| Refero fal docs screen `a2db66c1-30cb-4df4-be39-533daf4d3d34` | 좁은 sticky 목차와 넓은 기술 자료 | 표·code·구성도를 prose보다 넓게 사용 |
| Refero shadcn UI style `c14c0a94-1037-449e-bf5b-4cb972656ac7` | compact density와 얇은 경계 | 기존 문서의 compact navigation과 border 유지 |

Reference lock은 현재 PureCVisor token, Pretendard, 공통 좌측 읽기 축과 모바일 reflow다.
외부 사례의 색상, 브랜드 자산, 문구와 장식은 복제하지 않는다.

## 4. 후보 비교

1920px의 데이터베이스·네트워크 page DOM에 후보 token을 주입해 같은 viewport에서 비교했다.

| 후보 | prose | technical | canvas | sidebar | 1920px 실제 canvas | 판정 |
|---|---:|---:|---:|---:|---:|---|
| 기존 | 46rem | 60rem | 75rem | 19rem | 1,200px | 기각 — 좌·우 잔여 공간이 큼 |
| 균형 확장 | 54rem | 68rem | 84rem | 17rem | 1,312px | 보류 — 기술 자료 확장 여지 잔존 |
| 본문 우선 | 56rem | 72rem | 84rem | 16rem | 1,328px | 보류 — figure가 가용 폭을 모두 사용하지 못함 |
| 최대 활용 | 56rem | 72rem | 88rem | 16rem | 1,360px | 채택 — 24px 양쪽 gutter만 남기고 중앙 폭 사용 |

모든 후보에서 공통 좌측축 차이와 page-level overflow는 0이었다. 채택안은 1920px에서
Starlight가 제공하는 중앙 가용 폭을 모두 사용하면서 일반 문장은 896px로 제한한다.

## 5. 결정 ledger

| 결정 | 이유 |
|---|---|
| reader sidebar를 19rem에서 16rem으로 축소 | 탐색 label을 유지하면서 중앙 폭 96px 추가 확보 |
| 일반 본문을 46rem에서 56rem으로 확장 | 한국어 문장 몰입을 해치지 않는 896px 상한에서 빈 공간 축소 |
| 네트워크 page의 43rem 예외 제거 | 모든 문서가 같은 prose 폭과 읽기 축을 사용 |
| 기술 자료를 56~72rem 적응형으로 확장 | 표 열 줄바꿈과 code 내부 scroll을 줄이고 짧은 자료의 빈 박스 방지 |
| 아키텍처 canvas를 최대 88rem으로 확장 | 1920px reader의 실제 중앙 가용 폭 1,360px 사용 |
| reader rail inset 상한을 7.5rem에서 4rem으로 축소 | 넓은 canvas 안의 prose 시작점을 과도하게 안쪽으로 밀지 않음 |
| landing의 75rem shell 유지 | 이번 요청 범위인 좌·우 reader·본문 공간만 조정하고 landing 계층은 보존 |

## 6. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 1920px에서 좌·우 rail 기준 폭은 256px, reader canvas는 1,360px다. |
| P0 | 일반 본문·H1·pagination은 896px, 기술 자료는 896~1,152px를 사용한다. |
| P0 | H1~H6·본문·표·code의 공통 좌측축 차이는 0이다. |
| P0 | 데이터베이스·네트워크 page의 1920·1440·390px page overflow는 0이다. |
| P1 | 1280px 본문 가용 폭은 624px에서 720px로 늘고 390px는 358px를 유지한다. |
| P1 | mobile 표 focus와 방향키 내부 scroll, light·dark theme와 navigation을 유지한다. |
| P1 | Axe WCAG A/AA와 console·page·request 오류가 0이다. |

## 7. 구현

- `--pcv-prose-width`, `--pcv-technical-width`, `--pcv-architecture-width`를 각각
  `56rem`, `72rem`, `88rem`으로 조정했다.
- `--sl-sidebar-width`를 `16rem`, reader rail inset 상한을 `4rem`으로 조정했다.
- 네트워크 page의 `43rem` prose override를 제거하고 lead도 공통 prose token을 사용한다.
- artifact gate가 새 폭 token, sidebar와 네트워크 공통 폭 계약을 검사한다.

## 8. 로컬 검증

`cd site && npm run check`가 26개 page와 105개 Pages artifact를 생성·검증했다.

Chromium 152에서 데이터베이스와 네트워크 page를 측정했다. 1920px에서 좌·우 rail은 각각
256px, Markdown canvas는 `x=280`, `width=1360`이었다. 일반 본문은 `x=344`, `width=896`,
데이터베이스 첫 표는 `width=1152`, 네트워크 첫 표는 콘텐츠 기반 `width=1133`이었다.
두 page 모두 공통 좌측축 차이, page overflow, Axe 위반과 browser 오류가 0이었다.

1440px reader는 880px, 1280px reader는 720px로 가용 폭에 맞춰 줄었다. 390px에서는 기존과
같은 358px 단일 열을 사용했고 page overflow는 0이었다. 데이터베이스 표는 keyboard focus 후
방향키로 `scrollLeft 0→80px` 이동했다. `최신 릴리스 기준`은 896px 본문에서도 명시적 세 줄을
유지했다.

로컬 시각 증거 SHA-256은 다음과 같다.

- Chromium 1920px 데이터베이스:
  `4f8deca19fcba30ef60356a57e83393d5fc98ff93b8c54e2d8f6dd845e384d5c`
- Chromium 1920px 네트워크:
  `fe18fabfdfbfff6dd0c2c70fb89367bc284f21a9e833237e26cabfbe2363a013`
- Chromium 390px 데이터베이스:
  `239e48bc3518ef0a60810f3823ef2e00b7ce3d1139ece359824c1e09d051a9cb`
- Chromium 1920px 최신 릴리스:
  `d9cbce0e2986fdae724884df562d9785c1fe8a0b768045811c0bddb843d8d086`

로컬 수용 기준을 모두 충족해 Pages 배포 검증을 진행했다.

## 9. GitHub Pages·custom domain 검증

구현 commit `fe7ce8c9f35fd1b70493efe2a0a7c9e29f5dc1ff`의 GitHub Pages run
[`33425021822`](https://github.com/HardcoreMonk/purecvisor/actions/runs/33425021822)은 build와
deploy를 모두 성공했다.

`https://purecvisor.site`에서 데이터베이스와 네트워크 page를 다시 측정했다. 1920px의 좌·우
rail 256px, Markdown canvas 1,360px, 일반 본문 896px, 데이터베이스 표 1,152px와 네트워크 표
1,133px가 로컬 결과와 일치했다. 390px은 358px 단일 열을 유지했다. 두 viewport와 두 page에서
공통 좌측축 차이, page overflow, Axe 위반과 console·page·request 오류는 모두 0이었다.

라이브 1920px 데이터베이스·네트워크 캡처 SHA-256도 각각
`4f8deca19fcba30ef60356a57e83393d5fc98ff93b8c54e2d8f6dd845e384d5c`,
`fe18fabfdfbfff6dd0c2c70fb89367bc284f21a9e833237e26cabfbe2363a013`로 로컬과 일치했다.
모든 수용 기준을 충족했으므로 판정을 `LIVE-PASS`, ADR 상태를 `Verified`로 전환한다.
