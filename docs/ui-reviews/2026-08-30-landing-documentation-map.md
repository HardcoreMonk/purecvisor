# 공개 랜딩 문서 살펴보기 제품 UI 리뷰

> **일자:** 2026-08-30
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩의 `문서 살펴보기`
> **관련 기준:** `DESIGN.md`, `docs/PUBLIC_DOCUMENTATION_SITE.md`

## 1. 목표와 범위

- 사용자: 설치, 운영, API 또는 개발 문서로 바로 이동하려는 Single Edge 운영자와 개발자
- 목표: Hero와 상단 disclosure만 제공하던 랜딩에 전체 문서 맵을 추가해 8개 그룹·23개 정본
  문서를 첫 페이지에서 직접 탐색하게 한다.
- 비범위: 문서 본문, 상세 아키텍처, 상단 navigation, 제품 Web UI와 Multi Edge 비공개 경계
- 사용자 결정: opencodex.me 랜딩의 `문서 살펴보기` 기능을 GitHub Pages 랜딩에 반영한다.
- 이 결정은 `2026-08-24-landing-documentation-section-removal.md`의 landing directory 제거 판정을
  현행 요청 범위에서 대체한다. 당시 제거한 역할별 추천 경로와 반복 최종 CTA는 재도입하지 않는다.

## 2. 연구와 레퍼런스 잠금

| 출처 | 관찰 | 적용 |
|---|---|---|
| [opencodex.me](https://opencodex.me/) | 서비스 소개 뒤에 전체 문서를 그룹 제목과 단순 링크 목록으로 노출하고 4→2→1열로 재배치한다. | 전체 문서 맵의 정보 구조와 반응형 단계만 채택한다. |
| Refero Tailscale style `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 인프라 제품에 맞는 밝은 canvas, soft-gray surface, compact type와 낮은 장식을 사용한다. | section surface와 밀도의 보조 근거로 사용한다. |
| Refero PlanetScale style `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 1px divider와 연속 grid가 많은 항목을 구조적으로 묶는다. | 8개 그룹을 개별 카드 대신 하나의 hairline grid로 묶는다. |
| Refero Expo style `87dac905-fe95-414e-bca7-3698dcd1e0a8` | neutral canvas에서 색을 interaction에만 제한하고 그림자 없이 구획한다. | teal을 번호·link·focus에만 유지하고 그림자를 추가하지 않는다. |
| Refero HTTPie Docs `a01522c6-b9c9-472c-b753-ced6e8d945e2` | 개발 문서 진입점을 적은 수의 명확한 링크로 구분한다. | 짧은 명사형 link label과 직접 route를 유지한다. |
| Refero Anthropic Support `16e17aa5-5a50-4596-9863-72108da72c5a` | category 탐색을 일관된 grid와 충분한 hit area로 제공한다. | 그룹 heading과 mobile link target의 근거로 사용한다. |

기준 방향은 기존 PureCVisor의 Pretendard, 흰 canvas, soft gray, ink, teal token과 최대 8px
radius다. 외부의 색상, 서체, logo, 고유 문구, 이미지, pill과 개별 card 처리는 가져오지 않는다.
미디어는 필요하지 않으며 정보 구조 자체를 code-native HTML로 제공한다.

## 3. 결정 기록

| 결정 | 근거 | 역할과 이유 |
|---|---|---|
| Hero 다음에 문서 맵 배치 | 사용자 요청, opencodex.me | 소개에서 실제 문서 탐색으로 바로 이어지게 한다. |
| 8개 그룹·23개 링크 전체 노출 | `guide-routes.mjs` | 검색어를 모르는 사용자도 모든 정본 진입점을 훑을 수 있게 한다. |
| manifest 기반 공용 Astro 컴포넌트 | 공개 문서 동기화 계약 | root·KO·EN에서 route 목록이 갈라지지 않게 한다. |
| 단일 surface와 1px 구획 | PureCVisor token, PlanetScale | 카드 남용 없이 그룹 관계를 보존한다. |
| 4→2→1열 | opencodex.me, 23개 링크의 실제 밀도 | desktop scan과 mobile reflow를 함께 만족한다. |
| 그룹 번호와 H2→H3 계층 | 접근성·스캔 규칙 | 색상에 의존하지 않고 그룹 순서와 heading 구조를 제공한다. |
| 44px link target과 실제 `<a>` | `DESIGN.md`, craft 접근성 규칙 | keyboard, touch, 새 탭과 브라우저 기본 동작을 보존한다. |

## 4. 구현 계약

- `site/src/components/DocumentationMap.astro`가 `readerDocuments`를 그룹화한다.
- 한국어 root source를 `/ko/`로 복제할 때 `prepare-content.mjs`가 생성 위치에 맞는 component
  import 경로를 사용한다.
- 한국어와 영어 landing은 heading·설명·표시 label만 현지화하며 모든 link는 한국어 정본
  `/ko/<분류>/<문서>/`를 가리킨다.
- 문서 맵은 별도 JavaScript, 외부 runtime asset, image와 source map을 만들지 않는다.
- `check-site.mjs`는 세 landing의 section 순서, accessible name, 그룹·link 수, 모든 정본 href,
  surface token, 4→2→1열과 44px target을 검사한다.

## 5. 구현 후 검증

- `cd site && npm run check`: PASS, 27 page·92 artifact와 전체 내부 link 무결성 확인
- Chromium 1440×1000: 4열, 그룹 8개, link 23개, page horizontal overflow 0
- Chromium 900×900: 2열, 그룹 8개, link 23개, page horizontal overflow 0
- Chromium 390×844: 1열, 그룹 8개, link 23개, page horizontal overflow 0
- root·`/ko/`·`/en/`과 light·dark 조합: HTTP 200, 빈 label·잘못된 `aria-labelledby` 0
- 23개 link의 최소 계산 높이: 44px
- 첫 link keyboard focus: 3px solid outline
- Axe WCAG 2 A/AA: desktop·mobile violation 0
- console·page error: desktop·mobile 0
- light·dark visual comparison: 기존 canvas/soft/ink/teal 역할 유지
- Starlight 중첩 section 기본 여백을 0으로 고정해 grid row gap을 1px로 유지
- Hero와 문서 band 사이의 Starlight section 기본 여백을 0으로 고정해 surface를 연속 배치
- 실제 GitHub Pages 배포와 custom domain 재검증은 이 로컬 구현 범위에 포함하지 않는다.
