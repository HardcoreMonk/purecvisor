# 공개 랜딩 Single Edge 기능 지도 개선 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` 공개 랜딩 Hero의 Single Edge 다이어그램
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, `docs/PUBLIC_RELEASE_BOUNDARY.md`, ADR-0046

## 1. 제품 맥락과 목표

- 사용자: 첫 페이지에서 PureCVisor Single Edge의 실제 제공 범위를 빠르게 판단하는 운영자와
  기술 의사 결정자
- 핵심 작업: 접근 인터페이스, 제어면과 주요 워크로드·스토리지·네트워크 기능을 한 화면에서
  확인한 뒤 퀵스타트나 운영 가이드로 이동한다.
- 변경 목표: 현재 3개 하위 상자에 빠져 있는 Linux Bridge, Local VPC, VXLAN Overlay 등
  공개 기능을 보완해 다이어그램을 소개 자료로 사용할 수 있는 기능 지도로 개선한다.
- 비범위: Hero 설명·action, 문서 directory, 공개 범위 밖 Multi Edge·클러스터 자동화와 아직
  공개 지원 절차가 아닌 OVN Local VPC backend 후보는 추가하지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로: `.scratch/ui-reviews/2026-08-24-landing-capability-map/before-ko-hero.png`
- SHA-256: `d191049abd7eb534356d7320857d0f59cec7ea011476c5cfddfb41684a580226`
- 현재 동작·데이터 계약: Web UI·REST API·`pcvctl`에서 `purecvisorsd`를 거쳐 Compute,
  Storage, Network로 이어지는 구조다. 하위 기능은 KVM·LXC, ZFS·iSCSI, OVS·OVN만 표시하고
  전체 영역은 `role="img"`로 평면화되어 세부 텍스트를 보조 기술이 직접 탐색하지 못한다.

## 3. 기능 정본 조사

| 출처 | 확인된 공개 기능 | 적용 여부 |
|---|---|---|
| 사용자 결정 | Bridge, VPC, VXLAN 등이 빠져 소개 자료로 부적절함 | 채택, 정보 구조 개선의 직접 근거 |
| `docs/PUBLIC_RELEASE_BOUNDARY.md` | Linux bridge, OVS/OVN, Local VPC, VXLAN overlay core와 단일 노드 UI/API/CLI | 채택, 공개 기능 표기의 상한선 |
| `docs/GUIDE.md` VM·컨테이너·스토리지 장 | KVM VM, LXC, snapshot·clone, ZFS pool·zvol, iSCSI, backup·restore | 채택, 워크로드·스토리지 영역 |
| `docs/GUIDE.md` 네트워크 장 | Linux Bridge, OVS/OVN, VLAN, QoS, Local VPC, VXLAN Overlay, firewall·security group | 채택, 네트워크 2개 영역 |
| `docs/GUIDE.md` 운영·보안 장 | RBAC·audit, 비동기 job, monitor·alert, self-healing | 채택, 제어면 공통 기능 |

## 4. Refero 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| Vercel Infrastructure | [`d994631b-6d7a-4e07-8aa0-07788706b57d`](https://refero.design/pages/d994631b-6d7a-4e07-8aa0-07788706b57d) | 어두운 인프라 소개 화면에서 기능을 동일한 규칙의 반응형 grid로 분리해 범위를 빠르게 비교하게 함 | 제어면 아래 기능 영역을 2×2 grid로 나누는 근거로 채택 |
| Appwrite | [`5ceac5f5-9b89-4d1d-9d3f-83c431be37fc`](https://refero.design/pages/5ceac5f5-9b89-4d1d-9d3f-83c431be37fc) | Hero의 제품 시각 자료와 기능 grid가 같은 어두운 surface·accent 규칙을 공유함 | 기존 Hero token을 보존하며 정보량을 늘리는 근거로 채택 |
| Teenage Engineering Modules | [`47b7d79e-1c75-4390-950b-5fb51ac1e7f5`](https://refero.design/pages/47b7d79e-1c75-4390-950b-5fb51ac1e7f5) | 복잡한 기술 모듈을 같은 크기의 panel과 짧은 label로 묶어 신호·기능 범위를 정돈함 | 긴 설명 대신 도메인명과 3개 핵심 capability를 쓰는 근거로 부분 채택 |

기존 PureCVisor dark/teal 시각 체계는 승인돼 있으므로 새 style을 혼합하지 않고, Refero에서는
복잡한 기술 기능을 그룹화하는 정보 구조만 채택했다.

## 5. 결정

### 채택

- 다이어그램 제목을 `Single Edge capability map`으로 바꿔 내부 아키텍처보다 제공 기능을
  설명하는 목적을 명시한다.
- `Web UI · REST API · pcvctl` 접근층과 `purecvisorsd` 제어면 흐름은 유지한다.
- 제어면에는 사용자 관점의 공통 기능 `RBAC · Audit`, `Jobs · Alerts`, `Self-healing`을 표시한다.
- 하위 기능을 워크로드, 스토리지, 네트워크 패브릭, 가상 네트워크 4개 영역으로 나눈다.
  각 영역은 다음 3개 capability를 표시한다.
  - 워크로드: KVM VM, LXC, Snapshot·Clone
  - 스토리지: ZFS Pool·Zvol, iSCSI, Backup·Restore
  - 네트워크 패브릭: Linux Bridge, OVS·OVN, VLAN·QoS
  - 가상 네트워크: Local VPC, VXLAN Overlay, Firewall·Security Groups
- `role="img"`와 하위 `aria-hidden` 평면화를 제거하고 `figure`, `figcaption`, 목록 의미를
  사용해 보조 기술도 실제 기능을 탐색할 수 있게 한다.

### 기각

- 모든 RPC나 세부 운영 모드를 한 Hero에 나열하지 않는다. 4개 영역당 3개 대표 capability로
  소개 밀도를 제한하고 상세 계약은 운영 가이드로 연결한다.
- Multi Edge, cluster HA, live migration, VPC peering, floating IP와 OVN Local VPC backend를
  일반 공개 지원 기능처럼 표시하지 않는다.
- 새 illustration이나 외부 브랜드 자산을 추가하지 않는다. 기존 code-native panel 체계를
  확장한다.

## 6. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 누락 기능 보완 | root·`/ko/`·`/en/`에 Linux Bridge, Local VPC, VXLAN Overlay와 4개 기능 영역이 존재한다. |
| P0 | 공개 경계 준수 | 금지 기능이나 OVN Local VPC backend 지원 주장이 다이어그램에 없다. |
| P1 | 소개 자료 판독성 | 접근층 → 제어면 → 2×2 기능 grid 순서가 1440px에서 한 Hero 안에 읽히고 label이 겹치지 않는다. |
| P1 | 반응형 | 390px에서는 기능 영역이 한 열로 전환되고 horizontal overflow와 잘림이 없다. |
| P1 | 접근성 | figure 제목과 기능 목록을 보조 기술이 읽을 수 있고 axe·browser 오류가 없다. |
| P2 | 회귀 방지 | 정적 gate가 4개 영역, 필수 capability와 폐기된 3칸 resource 구조의 부재를 검사한다. |

## 7. 접근성·반응형·상태 검토

- keyboard/focus: 다이어그램은 비상호작용 정보이므로 tab stop을 추가하지 않는다.
- 의미 구조: `figure`의 `figcaption`과 실제 텍스트 목록을 사용하고 색상만으로 영역을 구분하지
  않는다.
- loading/empty/error/disabled: 정적 정보 지도이므로 해당 상태는 없다.
- 1024/768/480px: Hero grid 전환, 2×2에서 1열로 바뀌는 capability panel, 긴
  `Firewall · Security Groups`의 줄바꿈과 page overflow를 확인한다.

## 8. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 누락 기능과 공개 경계가 정본으로 결정된 정보 구조 수정이며 복수 시안의 시선 계층
  비교가 아니다.

## 9. 구현 후 검증

- UI 자동 테스트: `npm run check`가 26개 HTML과 총 86개 Pages artifact, 내부 link, 접근 가능한
  `figure` 제목, 4개 기능 영역과 필수 capability, 폐기된 `pcv-map-resources` 구조·CSS 부재를
  검증했다.
- 저장소 게이트: `python3 scripts/check_design_md.py`,
  `bash tests/integration/test_design_md_surface.sh`, `make check-public-comments`,
  `python3 scripts/strip_source_comments.py --check`, `git diff --check`가 통과했다.
- Chromium `1440 × 1000`: `/`, `/ko/`, `/en/`에서 접근층 → 제어면 → 2×2 기능 grid가 한
  Hero에서 읽히고 4개 영역, Linux Bridge, Local VPC, VXLAN Overlay와 나머지 필수 capability가
  모두 표시됨을 확인했다.
- Chromium `390 × 844`: 4개 기능 영역이 한 열로 전환되고 map·card·page horizontal overflow와
  텍스트 잘림이 없음을 확인했다.
- 모든 route·viewport에서 map은 `figure`와 연결된 `figcaption`을 사용하고 폐기된
  `role="img"` 평면화와 `pcv-map-resources`가 0건임을 확인했다.
- root·한국어·영어의 desktop·mobile axe 위반 0, browser console·page·request 오류 0을
  확인했다. 문서 8개 category·22개 chapter와 landing section 순서도 유지됐다.
- desktop 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map/after-ko-desktop.png`
  — `841a689524a489aea0b0cde227d433a44fa466e542f957ee1724dd59bbe22bc0`
- mobile 캡처:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map/after-ko-mobile.png`
  — `31a04c1fc09ff8f33b7832d09e2ed1b1271848a15d95d6b61b6efdad4713bf29`
- 잔여 위험: 새 공개 기능을 추가하거나 지원 경계를 바꿀 때 기능 지도의 대표 capability도
  함께 검토해야 한다. 현재 필수 기능 누락과 폐기 구조 재도입은 `npm run check`가 차단한다.
- 구현 commit `d41560f`의 GitHub Pages run
  [`32658233655`](https://github.com/HardcoreMonk/purecvisor/actions/runs/32658233655)이 성공했다.
- `purecvisor.site`의 `/`, `/ko/`, `/en/`을 1440px·390px에서 확인한 결과 기능 지도 4개 영역,
  Linux Bridge, Local VPC, VXLAN Overlay와 2열·1열 반응형 전환이 운영 산출물에 반영됐다.
- 운영 domain의 map·page overflow, axe 위반과 browser console·page·request 오류는 모두 0이며
  운영 screenshot SHA-256은 로컬 검증 screenshot과 일치한다.
