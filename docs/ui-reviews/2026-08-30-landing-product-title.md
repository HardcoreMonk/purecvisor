# 랜딩 제품명 Hero UI 리뷰

> **일자:** 2026-08-30
> **판정:** PASS — 로컬 production build·브라우저 검증 완료
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩 Hero
> **관련 기준:** `DESIGN.md`, `docs/PUBLIC_DOCUMENTATION_SITE.md`

## 1. 목표와 범위

- 사용자 결정: `PURECVISOR 2.0.0 · SINGLE EDGE` eyebrow를 삭제하고 Hero H1을
  `PURECVISOR 2.0.0`으로 변경한다.
- 목표: 중복된 제품·배포 범위 표현을 줄이고 제품명을 Hero의 유일한 최상위 제목으로 제공한다.
- 비범위: lead, CTA, 배포 note, header, 문서 맵, 색상, 서체와 반응형 구조 변경
- 적용 경로: 한국어 원본이 생성하는 `/`·`/ko/`와 영어 `/en/`

## 2. 연구와 레퍼런스 잠금

| 출처 | 관찰 | 적용 |
|---|---|---|
| 사용자 명시 요청 | eyebrow 삭제와 H1 문구를 정확히 지정했다. | 최우선 콘텐츠 계약으로 채택한다. |
| Refero Tailscale style `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 기술 제품 Hero는 간결한 대형 제목과 compact supporting copy로 위계를 만든다. | 제품명을 유일 H1으로 두고 기존 lead를 보조 문장으로 유지한다. |
| Refero PlanetScale style `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 단일 기술 headline과 강한 흑백 대비가 제품 정체성을 직접 전달한다. | 별도 version label을 반복하지 않는다. |
| Refero HashiCorp style `8f7b11aa-1c27-40be-a463-dd9c16a41769` | 대형 display type과 짧은 보조 copy가 기술 제품의 주제목을 분명히 한다. | 제목 위계만 참고하고 gradient·imagery·외부 token은 사용하지 않는다. |

기준 방향은 현행 PureCVisor Hero다. Pretendard, graphite surface, 흰 제목, teal focus와 CTA 구조를
그대로 유지한다. 새 시각 요소와 외부 브랜드의 색상·서체·문구는 도입하지 않는다.

## 3. 결정 기록

| 결정 | 근거 | 이유 |
|---|---|---|
| 세 landing의 H1을 `PURECVISOR 2.0.0`으로 통일 | 사용자 요청, locale 구조 | 경로별 제품명 위계가 갈라지지 않게 한다. |
| 제품 eyebrow 요소 삭제 | 사용자 요청 | H1과 중복되는 version·edition label을 제거한다. |
| Hero H1의 상단 margin을 0으로 조정 | 제거된 선행 요소 | 빈 간격 없이 기존 Hero 시작선에 제목을 배치한다. |
| 이전 eyebrow와 두 H1 문구를 build gate에서 금지 | 회귀 계약 | 생성되는 `/ko/`와 영어 landing까지 이전 문구 재등장을 차단한다. |
| 공용 `.pcv-eyebrow`는 유지 | 문서 맵 label | `DOCUMENTATION · 23 DOCUMENTS`의 의미와 스타일은 변경 범위가 아니다. |

## 4. 수용 기준

- `/`, `/ko/`, `/en/`마다 H1이 정확히 하나이고 값은 `PURECVISOR 2.0.0`이다.
- `PURECVISOR 2.0.0 · SINGLE EDGE`, 한국어·영어 이전 H1은 세 landing에 존재하지 않는다.
- lead, CTA 2개, 배포 note와 문서 맵은 유지된다.
- desktop·mobile과 light·dark에서 가로 overflow, 접근성 위반과 runtime 오류가 없다.
- site build, UI 디자인 계약, 공개 source comment gate가 통과한다.

## 5. 구현 및 검증 결과

- 한국어·영어 landing source에서 제품 eyebrow를 제거하고 H1을 `PURECVISOR 2.0.0`으로 통일했다.
- 생성되는 `/ko/`를 포함한 세 landing에서 이전 eyebrow와 한국어·영어 H1을 금지하는 site gate를
  추가했다.
- Hero 전용 eyebrow color rule을 제거하고 H1 상단 margin을 `0`으로 조정했다. 문서 맵의 공용
  eyebrow와 8개 그룹·23개 link는 유지했다.

Astro production preview에서 `/`, `/ko/`, `/en/` × light·dark × 1440×1000·390×844의 12개
조합을 검사했다.

| 항목 | 결과 |
|---|---|
| HTTP·언어·theme | 12개 조합 모두 200, `lang`과 선택 theme 일치 |
| Hero 제목 | route별 H1 1개, 값 `PURECVISOR 2.0.0`, 이전 문구 0 |
| Hero 구조 | eyebrow 0, CTA 2개, 배포 note 1개, H1 margin-top 0 |
| 반응형 | page·Hero·H1 가로 overflow 0, CTA 최소 높이 48px |
| 문서 맵 보존 | route별 문서 link 23개 |
| 접근성·안정성 | Axe WCAG 2 A/AA 위반 0, console·page·request 오류 0 |

대표 Hero 캡처는 `.scratch/ui-reviews/2026-08-30-landing-product-title/`의
`hero-light-desktop.png`, `hero-dark-desktop.png`, `hero-light-mobile.png`,
`hero-dark-mobile.png`에서 비교했다. light·dark와 desktop·mobile 모두 제품명이 기존 시작선에
정렬되고 lead·CTA·배포 note의 위계가 유지된다.

저장소 검증 결과:

- `cd site && npm run check`: PASS — 27 page, Pages artifact 92 file
- `make single`, `make release`: PASS — C23 경고 0
- `make test`: PASS — g_test 1,370개, audit startup 5/5
- `make check-all`: PASS — 공개 계약 38개 gate
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `check_ui_bundle_fresh.py`: PASS
- `check_design_md.py`, `test_design_md_surface.sh`: PASS
- `strip_source_comments.py --check`, `git diff --check`: PASS
