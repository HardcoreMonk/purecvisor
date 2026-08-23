# 공개 랜딩 Hero 문구 축소 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩 Hero
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046

## 1. 제품 맥락과 목표

- 사용자: PureCVisor 공개 랜딩에서 제품 범위와 운영 문서 진입을 확인하는 Single Edge 운영자
- 핵심 작업: 과장된 슬로건 없이 제품 범위 문장을 읽고 퀵스타트나 운영 가이드로 이동한다.
- 변경 목표: 사용자 요청에 따라 `하나의 노드, 하나의 제어면` 제목과 기존 네트워크 설명을
  삭제하고 `VM, 컨테이너, ZFS 스토리지와 네트워크 가상화를 하나의 Linux/KVM 노드에서
  운영합니다.`로 교체한다.
- 비범위: 제품 버전 label, CTA, 제어면 구조, 문서 directory와 Header navigation은 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로: `.scratch/ui-reviews/2026-08-24-landing-hero-copy/before-ko-hero.png`
- SHA-256: `e565ad690f19bcdc1349b419f7821f1376d004dce232b9819e8868ed63085876`
- 현재 동작·데이터 계약: `pcv-hero-title` H1이 큰 슬로건을 표시하고 Hero section의
  `aria-labelledby`를 제공한다. 바로 아래 설명은 소프트웨어 정의 네트워크라는 표현을 사용한다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 결정 | 2026-08-24 요청 | 기존 Hero 제목 삭제와 네트워크 설명의 정확한 교체 문구를 지정함 | 채택, copy와 시각 계층 변경의 직접 근거 |
| 기존 랜딩 축소 리뷰 | `docs/ui-reviews/2026-08-24-landing-section-removal.md` | Hero 뒤 마케팅 구역을 제거하고 문서 탐색을 앞당김 | 정보 밀도 축소 방향과 CTA·문서 directory 유지 근거로 채택 |
| `DESIGN.md` | Hero typography 규칙 | hero-scale 제목은 실제 Hero에만 허용하고 장식보다 판독성을 우선함 | 큰 슬로건 제거 후 제품 label과 본문을 분리하는 근거로 채택 |
| Refero Fernand Docs | [`54c4c424-f096-4361-b07c-cdff6263aa12`](https://refero.design/pages/54c4c424-f096-4361-b07c-cdff6263aa12) | 문서 진입 화면은 제품 식별, 짧은 설명과 category 탐색을 중심으로 구성함 | 제품 식별 H1과 짧은 범위 설명 유지 근거로 부분 채택 |

새 외부 시각 방향을 만들지 않는 기존 랜딩의 후속 copy 축소이므로 승인된 이전 Refero 조사와
로컬 리뷰를 재사용했다.

## 4. 결정

### 채택

- 한국어 큰 H1 문장을 DOM에서 제거하고 영어 대응 문장 `One node, one control plane`도 제거한다.
- 기존 제품 label `PURECVISOR 2.0.0 · SINGLE EDGE`를 작은 시각 크기 그대로 유일한 H1으로
  승격하고 `pcv-hero-title` id를 이전해 section의 accessible name을 유지한다.
- 한국어 설명은 사용자 지정 문장으로 정확히 교체한다. 영어 설명은 같은 의미의
  `Operate VMs, containers, ZFS storage, and network virtualization on one Linux/KVM node.`로
  동기화한다.
- 제거된 대형 H1 전용 CSS와 반응형 override, 정적 build의 기존 제목 긍정 assertion을 제거하고
  새 문장·유일 H1·구문 부재를 검사한다.

### 기각

- H1을 완전히 없애지 않는다. 시각적으로 제목을 줄여도 문서 이름과 Hero의 accessible name은
  유지해야 한다.
- 새 슬로건, illustration, 빈 공간 보상용 card를 추가하지 않는다.
- 새 문장을 대형 제목으로 승격하지 않는다. 사용자가 기존 설명 문장의 교체 문구로 지정했다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 한국어 문구 삭제·교체 | root·`/ko/`에 두 삭제 문장이 없고 새 문장이 정확히 한 번 존재한다. |
| P0 | 영어 정보 구조 동기화 | `/en/`에 대응 제목·기존 설명이 없고 network virtualization 문장이 존재한다. |
| P0 | heading 의미 유지 | 각 landing에 `pcv-hero-title`인 H1이 정확히 하나 있고 제품 label을 accessible name으로 제공한다. |
| P1 | 시각 잔여 제거 | 대형 Hero H1 전용 CSS가 남지 않고 CTA·제어면 구조가 기존 위치를 유지한다. |
| P1 | 접근성·반응형 | 1440px·390px에서 overflow가 없고 axe와 browser 오류가 없다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: CTA와 Header keyboard 계약은 변경하지 않는다.
- 색상 외 상태 표현: 제품 label은 텍스트 H1이며 색상만으로 제품을 식별하지 않는다.
- loading/empty/error/disabled: 정적 Hero이므로 해당 상태는 없다.
- 1024/768/480px: 대형 제목 제거 후 copy column이 지나치게 비어 보이지 않는지, CTA와 제어면
  구조의 수직 정렬 및 mobile overflow를 확인한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자 지정 copy 제거·교체이며 복수 시안의 시선 계층 비교가 아니다.

## 8. 구현 후 검증

- UI 자동 테스트: `npm run check`가 26개 HTML과 총 86개 Pages artifact, 내부 link,
  기존 Hero 문구 부재, 새 문장, landing별 유일 H1과 대형 H1 CSS 부재를 검증했다.
- 저장소 게이트: `python3 scripts/check_design_md.py`,
  `bash tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `python3 scripts/strip_source_comments.py --check`, `git diff --check`가 통과했다.
- Chromium `1440 × 1000`: `/`, `/ko/`, `/en/`에서 H1이 제품 label 하나이고 Hero
  `aria-labelledby`가 해당 id를 사용하며 기존 제목·설명은 0건임을 확인했다.
- 한국어는 사용자 지정 문장, 영어는 `network virtualization` 대응 문장을 정확히 표시한다.
- 새 한국어 문장에 `word-break: keep-all`을 적용해 `노드`가 음절 중간에서 갈라지지 않도록
  실제 캡처로 보정했다.
- Chromium `390 × 844`: 제품 label, 새 문장, 두 CTA와 제어면 구조가 유지되고 overflow 0임을
  확인했다.
- root·한국어·영어·mobile의 axe 위반 0, browser console·page·request 오류 0을 확인했다.
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-hero-copy/after-ko-desktop.png`
  — `d191049abd7eb534356d7320857d0f59cec7ea011476c5cfddfb41684a580226`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-hero-copy/after-ko-mobile.png`
  — `790f91f396201c8f6e76b80e0fd5a2c9ca40c720245767da2ba5df5feadaac70`
- 잔여 위험: commit·push·Pages 배포 전이므로 운영 domain은 이전 Hero 문구를 유지한다.
