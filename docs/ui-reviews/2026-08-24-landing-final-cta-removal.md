# 공개 랜딩 최종 CTA 제거 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩 하단
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046

## 1. 제품 맥락과 목표

- 사용자: 첫 페이지에서 제품 범위를 확인하고 8개 문서 카테고리·22개 장 중 필요한 운영
  절차로 이동하는 Single Edge 운영자
- 핵심 작업: Hero action이나 문서 directory link를 사용해 운영 가이드에 진입한다.
- 변경 목표: 사용자 요청에 따라 `한 노드부터 명확하게 운영하세요.`가 포함된 하단 최종 CTA
  구역 전체를 제거하고 문서 directory와 역할별 경로를 첫 페이지의 마지막 본문으로 만든다.
- 비범위: Hero, 상단 disclosure navigation, 문서 8개 카테고리·22개 장, 역할별 추천 경로와
  Starlight footer는 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로:
  `.scratch/ui-reviews/2026-08-24-landing-final-cta-removal/before-ko-final-cta.png`
- SHA-256: `8d01f65442315505ada0b4c879dfc4b3da933a09597c3cd00660ba24df66a4bc`
- 현재 동작·데이터 계약: 문서 directory와 역할별 경로 뒤에 어두운 `pcv-final-cta` 구역이
  같은 전체 운영 가이드와 GitHub action을 반복한다. 한국어와 영어 landing이 대응 구조를
  사용한다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | `한 노드부터 명확하게 운영하세요.` 섹터 삭제 | 채택, 구역 제거의 직접 근거 |
| 기존 랜딩 축소 리뷰 | `docs/ui-reviews/2026-08-24-landing-section-removal.md` | Hero 다음에 문서 directory를 직접 배치하고 반복 마케팅 구역을 제거함 | 문서 중심 정보 구조를 끝까지 유지하는 근거로 채택 |
| Refero Fernand Docs | [`54c4c424-f096-4361-b07c-cdff6263aa12`](https://refero.design/pages/54c4c424-f096-4361-b07c-cdff6263aa12) | 짧은 제품 식별 뒤 문서 카테고리 탐색을 중심 과업으로 구성함 | 문서 directory를 마지막 핵심 본문으로 유지하는 근거로 채택 |
| Refero HTTPie Docs | [`a01522c6-b9c9-472c-b753-ced6e8d945e2`](https://refero.design/pages/a01522c6-b9c9-472c-b753-ced6e8d945e2) | 문서 hero의 소수 정본 action과 문서 진입으로 탐색을 압축함 | Hero의 기존 action으로 충분하므로 하단 반복 CTA 제거 근거로 부분 채택 |

새 시각 방향을 설계하지 않는 기존 랜딩의 후속 삭제이므로 승인된 Refero 조사와 로컬 리뷰를
재사용했다.

## 4. 결정

### 채택

- 한국어 `한 노드부터 명확하게 운영하세요.`와 영어 대응 문장
  `Operate clearly, starting with one node.`가 포함된 `pcv-final-cta` section 전체를 제거한다.
- section 안의 반복 제품 kicker, 전체 운영 가이드와 GitHub action도 함께 제거하고 대체 구역을
  추가하지 않는다.
- Hero action, 문서 category link와 역할별 경로를 유지해 핵심 문서 진입 경로가 사라지지 않게
  한다.
- 불용 전용 CSS를 제거하고 root·한국어·영어 artifact에서 section과 문구가 재도입되면 실패하는
  정적 검증을 추가한다.

### 기각

- 제목만 숨기고 어두운 배경이나 버튼을 남기지 않는다. 사용자 요청의 `섹터 삭제`를 section
  전체 삭제로 적용한다.
- 제거 공간을 새 slogan, illustration 또는 다른 CTA로 채우지 않는다.
- Starlight의 공통 page footer는 사이트 구조이므로 제거 대상에 포함하지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 최종 CTA 제거 | root·`/ko/`·`/en/` HTML에 `pcv-final-cta`, `pcv-final-title`과 한국어·영어 대응 문장이 없다. |
| P0 | 정보 구조 정리 | 각 landing의 직접 section 순서가 `_top → documentation`이고 문서 뒤에 반복 CTA가 없다. |
| P1 | 문서 탐색 유지 | 8개 카테고리·22개 장, 역할별 경로와 Hero의 운영 가이드 action이 유지된다. |
| P1 | 반응형·접근성 | 1440px·390px에서 overflow가 없고 axe 및 browser 오류가 없다. |
| P2 | 불용 코드 제거 | `pcv-final-cta` 전용 CSS가 남지 않고 정적 gate가 재도입을 차단한다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 제거되는 두 action이 tab 순서에서 함께 사라지고 Hero·문서 link의 focus 계약은
  유지한다.
- 색상 외 상태 표현: 남는 문서 card와 역할별 경로는 텍스트·번호·link로 식별된다.
- loading/empty/error/disabled: 정적 landing이므로 해당 상태는 없다.
- 1024/768/480px: 문서 역할별 경로 뒤에 공통 footer가 자연스럽게 이어지는지와 horizontal
  overflow 여부를 확인한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자 지정 구역의 직접 삭제이며 복수 시안의 시선 계층 비교가 아니다.

## 8. 구현 후 검증

- UI 자동 테스트: `npm run check`가 26개 HTML과 총 86개 Pages artifact, 내부 link,
  landing별 `_top → documentation` section 순서, 최종 CTA 문구·ID·CSS selector 부재를
  검증했다.
- 저장소 게이트: `python3 scripts/check_design_md.py`,
  `bash tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `python3 scripts/strip_source_comments.py --check`, `git diff --check`가 통과했다.
- Chromium `1440 × 1000`과 `390 × 844`: `/`, `/ko/`, `/en/`의 직접 section 순서가
  `_top → documentation`이고 `pcv-final-cta`·`pcv-final-title`·삭제 문구가 0건임을 확인했다.
- 모든 route·viewport에서 8개 category·22개 chapter·3개 역할별 경로가 유지되고 horizontal
  overflow가 없음을 확인했다.
- root·한국어·영어의 desktop·mobile axe 위반 0, browser console·page·request 오류 0을
  확인했다.
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-final-cta-removal/after-ko-desktop.png`
  — `57ad5686d06b297edd4196c94e9e6a7526b8adb510e9cccbf2e6c0dbe43a2a8b`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-final-cta-removal/after-ko-mobile.png`
  — `2c38a02243979dd52e2ef79b2d5214a0853c2979f476554e3a9ec079019bdf6e`
- 잔여 위험: 없음. 최종 CTA section·문구·전용 CSS 재도입과 section 순서 회귀는
  `npm run check`가 차단한다.
- 구현 commit `d41560f`의 GitHub Pages run
  [`32658233655`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32658233655)이 성공했다.
- `purecvisor.site`의 `/`, `/ko/`, `/en/`을 1440px·390px에서 다시 확인한 결과 최종 CTA와
  삭제 문구는 0건이고 section 순서는 `_top → documentation`, 8개 category·22개 chapter·3개
  역할별 경로가 유지됐다.
- 운영 domain의 axe 위반, browser console·page·request 오류와 horizontal overflow는 모두
  0이다.
