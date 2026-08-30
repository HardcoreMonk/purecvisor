# 랜딩 멀티 제어면 문서 항목 제거 UI 리뷰

> **일자:** 2026-08-30
> **판정:** PASS — 로컬 production build·브라우저 검증 완료
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩의 `문서 살펴보기`
> **관련 기준:** `DESIGN.md`, `docs/PUBLIC_DOCUMENTATION_SITE.md`

## 1. 목표와 범위

- 사용자 결정: 랜딩 문서 맵에서 `멀티 제어면 참고 기록` 항목을 삭제한다.
- 목표: Single Edge 신규 방문자의 첫 문서 탐색에서 멀티 제어면 기록을 제외한다.
- 적용 범위: root·한국어·영어 landing의 문서 맵과 문서 수 label
- 보존 범위: `docs/GUIDE.md` 원문, 기존 정본 route, legacy hash redirect, reader sidebar와 검색
- 비범위: 문서 본문 삭제, route 제거, 그룹·grid·색상·타이포그래피·반응형 변경
- 이 결정은 `2026-08-30-landing-documentation-map.md`의 landing 23개 링크 노출 계약 중 해당 항목만
  대체하며 reader의 23개 문서 계약은 변경하지 않는다.

## 2. 연구와 레퍼런스 잠금

| 출처 | 관찰 | 적용 |
|---|---|---|
| 사용자 명시 요청 | 특정 문서 항목 하나를 삭제하도록 지정했다. | 랜딩 노출 목록에서만 정확히 제외한다. |
| 공개판 경계 | Single Edge 첫 진입은 단일 노드 운영 표면을 우선한다. | 멀티 제어면 기록을 랜딩 추천 목록에 노출하지 않는다. |
| Refero OpenAI Developers `44317718-1e56-45e0-8de3-7ede70f34349` | 개발자 진입점은 제한된 정보와 명확한 계층을 사용한다. | 불필요한 진입 링크를 제거하고 기존 구조를 유지한다. |
| Refero Tailscale `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 인프라 제품의 compact 문서 구조와 낮은 장식이 탐색을 돕는다. | 남은 링크의 밀도·행 높이·surface를 변경하지 않는다. |
| Refero Vectary `91b6dc03-35f1-461a-80b6-0b2473c05f5a` | 기술 문서는 타이포그래피·간격·구분선 중심의 위계를 사용한다. | 빈 placeholder나 대체 장식을 만들지 않는다. |

기준 방향은 기존 PureCVisor 문서 맵이다. 흰 canvas, soft-gray band, 1px grid, Pretendard,
teal focus와 4→2→1열을 그대로 유지한다. 외부 token, 이미지와 새 interaction은 도입하지 않는다.

## 3. 결정 기록

| 결정 | 근거 | 이유 |
|---|---|---|
| `landingDocuments`를 별도 export | 목록 노출과 reader route의 역할 분리 | 정본 문서 삭제 없이 세 landing을 한 manifest로 동기화한다. |
| `multi-control-plane-notes`만 filter | 사용자 요청 | 다른 22개 landing 진입점에 영향을 주지 않는다. |
| eyebrow 수를 manifest 길이로 출력 | 회귀 방지 | 목록과 `DOCUMENTATION · 22 DOCUMENTS`가 자동 동기화된다. |
| 8개 그룹 유지 | 인프라 그룹에 스토리지·네트워크가 남음 | 번호·heading hierarchy와 grid 구조를 보존한다. |
| build gate에서 경로·한영 label 금지 | 반사실 검증 | 해당 항목이 landing에 다시 들어오면 즉시 실패한다. |
| reader의 23개 route·sidebar 보존 | 최소 변경 범위 | 원문과 기존 bookmark를 깨지 않는다. |

## 4. 수용 기준

- `/`, `/ko/`, `/en/`의 문서 맵은 8개 그룹·22개 링크를 제공한다.
- `멀티 제어면 참고 기록`, `Multi-control-plane notes`와 해당 landing link가 존재하지 않는다.
- `/ko/infrastructure/multi-control-plane-notes/` 정본 page는 계속 빌드되고 직접 접근할 수 있다.
- reader sidebar의 8개 그룹·23개 링크와 legacy mapping은 유지된다.
- 4→2→1열, 44px link target, focus, light·dark와 무가로 overflow 계약이 유지된다.

## 5. 구현 및 검증 결과

- `readerDocuments`에서 landing 전용 `landingDocuments`를 파생하고
  `multi-control-plane-notes`만 제외했다.
- `DocumentationMap.astro`는 landing manifest로 8개 그룹·22개 링크를 렌더링하며 eyebrow의
  문서 수도 manifest 길이에서 계산한다.
- site gate는 세 landing의 22개 링크, 한영 label과 route 부재를 검사하고 reader의 23개
  route·sidebar·legacy mapping 검사는 그대로 유지한다.

Astro production preview에서 `/`, `/ko/`, `/en/` × light·dark × 1440×1000·900×900·390×844의
18개 조합을 검사했다.

| 항목 | 결과 |
|---|---|
| landing 문서 맵 | 모든 조합에서 8개 그룹·22개 링크, count label 22 |
| 제외 항목 | 한영 label과 `/ko/infrastructure/multi-control-plane-notes/` landing link 0 |
| 인프라 그룹 | 스토리지·네트워크 2개 링크 유지 |
| 반응형 | 1440px 4열, 900px 2열, 390px 1열, grid·page overflow 0 |
| 접근성 | link 최소 높이 44px, 3px solid focus, 잘못된 `aria-labelledby` 0 |
| 안정성 | Axe WCAG 2 A/AA 위반 0, console·page·request 오류 0 |
| reader 보존 | 제외 문서 직접 route 200, active item 유지, 고유 reader link 23개 |

대표 캡처는 `.scratch/ui-reviews/2026-08-30-landing-multi-control-plane-entry-removal/`의
`documentation-light-desktop.png`, `documentation-dark-desktop.png`,
`documentation-light-mobile.png`, `documentation-dark-mobile.png`에서 비교했다. 항목 제거 뒤에도
인프라 그룹과 전체 grid의 정렬, surface와 행 간격이 light·dark에서 유지된다.

저장소 검증 결과:

- `cd site && npm run check`: PASS — 27 page, Pages artifact 92 file
- `make single`, `make release`: PASS — C23 경고 0
- `make test`: PASS — g_test 1,370개, audit startup 5/5
- `make check-all`: PASS — 공개 계약 38개 gate
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `check_ui_bundle_fresh.py`: PASS
- `check_design_md.py`, `test_design_md_surface.sh`: PASS
- `strip_source_comments.py --check`, `git diff --check`: PASS
