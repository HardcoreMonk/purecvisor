# 공개 랜딩 Single Edge 아키텍처 레이어 제품 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 아키텍처 지도
> **관련 spec/plan:** `docs/PUBLIC_DOCUMENTATION_SITE.md`, ADR-0046,
> `docs/ui-reviews/2026-08-24-landing-capability-map-motion.md`

## 1. 제품 맥락과 목표

- 사용자: 공개 첫 화면에서 PureCVisor가 한 Linux/KVM 노드에 어떤 계층으로 구성되는지 판단하고
  해당 운영 가이드로 이동하는 운영자와 기술 의사 결정자
- 핵심 작업: 접근 인터페이스부터 제어면, 서비스, runtime adapter와 Linux host까지의 포함 관계를
  읽고 워크로드·스토리지·네트워크 상세 문서로 이동한다.
- 변경 목표: 사용자 지정 Claude 아키텍처 레이어 샘플의 세로 계층 문법을 채택하되, 내용은 현재
  `purecvisor-single`의 C23·`purecvisorsd`·Single Edge 공개 정본으로 다시 구성한다.
- 비범위: Hero 문구·action, 문서 directory, 상단 navigation, Multi Edge·cluster 기능과 runtime
  제품 Web UI는 바꾸지 않는다.

## 2. 현재 상태 증거

- 캡처 시각: 2026-08-24 Asia/Seoul
- 캡처 경로:
  `.scratch/ui-reviews/2026-08-24-landing-capability-map-motion/after-ko-desktop.png`
- SHA-256: `44869071e09942ecf03b15ee7b50466fcc21a209089ad89758872c8f1a7cb058`
- 현재 동작·데이터 계약: Web UI·REST API·`pcvctl`에서 `purecvisorsd`를 거쳐 4개 capability
  card로 분기한다. 기능 범위와 직접 link는 정확하지만 service 아래 runtime·host 계층과 포함
  관계가 없어 전체 아키텍처 소개 자료로는 깊이가 부족하다.

## 3. 연구 근거

| 출처 | ID/URL | 관찰 | 적용 여부 |
|---|---|---|---|
| 사용자 지정 Claude 공유 | [`5df29e48-c520-4dc7-9862-51475b63457d`](https://claude.ai/share/5df29e48-c520-4dc7-9862-51475b63457d) | 큰 container를 세로로 쌓고 각 layer 안에 같은 높이의 component를 배치해 전체 구조와 포함 관계를 함께 전달함 | 구조 문법을 주 근거로 채택, 내용·7색 범례·수치는 기각 |
| Eraser style | [`66b9ff99-f7f2-480c-97e9-42718e49cb97`](https://refero.design/styles/66b9ff99-f7f2-480c-97e9-42718e49cb97) | 어두운 diagram canvas, mono label과 선·surface 차이로 높은 기술 밀도를 정돈함 | 현재 dark/mono diagram의 component 밀도 근거로 부분 채택 |
| Timescale style | [`520e6739-69c0-4c16-a3ce-1c891c8c77c6`](https://refero.design/styles/520e6739-69c0-4c16-a3ce-1c891c8c77c6) | blueprint형 구조와 기능적 technical illustration이 복잡한 시스템 설명을 담당함 | 계층 경계·연결선의 설명 역할만 채택, 밝은 palette·offset shadow는 기각 |
| Vercel Infrastructure screen | [`d994631b-6d7a-4e07-8aa0-07788706b57d`](https://refero.design/pages/d994631b-6d7a-4e07-8aa0-07788706b57d) | dark infrastructure landing에서 제한된 accent와 icon을 사용해 제품 영역을 구분함 | 기존 teal accent와 code-native icon 유지 근거로 채택 |
| 기존 기능 지도 리뷰 | `docs/ui-reviews/2026-08-24-landing-capability-map-motion.md` | 실제 link, hover/focus 동등성, load 정지, reduced motion과 반응형 계약이 검증됨 | 상호작용·접근성 계약을 그대로 승계 |
| Single Edge 정본 | `docs/GUIDE.md`, `docs/PUBLIC_RELEASE_BOUNDARY.md`, 실제 `src/` module | C23 단일 프로세스 `purecvisorsd`, GMainLoop·GTask, VM/LXC, ZFS/iSCSI, bridge/OVS/OVN, Local VPC와 host integration이 현재 허용 범위임 | 표시 내용의 단일 진실로 채택 |

### Reference lock

- 주 방향: 사용자 지정 Claude 샘플의 세로 layer containment와 top-down flow
- 유지할 기존 signature: near-black canvas, 얇은 border, mono label, teal signal, 4개 서비스 icon
- 보조 차용: Eraser의 정보 밀도와 Vercel의 제한된 accent
- 명시적 기각: 7색 rainbow layer, gradient·glow 장식 확대, LOC·RPC·test 수치, C11·v0.9.5,
  `purecvisord`, cluster·etcd·live migration·Multi Edge 기능

## 4. 결정

### 채택

- 기능 card 3단 구조를 다음 5개 세로 layer로 재구성한다.
  1. Access: Web UI, REST API, `pcvctl`
  2. Control plane: `purecvisorsd`, C23·GMainLoop 단일 프로세스, UDS·REST·WebSocket,
     RPC dispatcher·GTask jobs, RBAC·Audit, Jobs·Alerts·Self-healing
  3. Capability services: VM·LXC, ZFS·iSCSI, Linux Bridge·OVS/OVN, Local VPC·VXLAN
  4. Runtime adapters: libvirt·KVM/QEMU, liblxc, ZFS·zvol, Linux Bridge·OVS/OVN,
     nftables·dnsmasq
  5. Linux host: Linux kernel, CPU·RAM, physical NIC, local disk·ZFS pool
- Access 3개와 capability service 4개만 실제 운영 가이드 link로 제공한다. Runtime과 host는
  계층 설명이므로 불필요한 tab stop을 만들지 않는다.
- service link hover와 `:focus-visible`에서 모든 layer connector와 control plane을 활성화하고,
  선택 서비스에 대응하는 runtime·host node를 함께 강조한다.
- 기존 inline SVG icon, card scan과 connector signal을 layer node에 맞게 재사용한다. page load에는
  animation이 없고 reduced motion에서는 transform과 반복 animation을 제거한다.
- root와 `/ko/`는 한국어 label, `/en/`은 영어 label을 사용하되 기술 식별자는 같은 표기를 유지한다.

### 기각

- Claude SVG를 iframe, image 또는 그대로 복제하지 않는다. 외부 의존과 사실 불일치, keyboard
  접근성 문제를 피하고 HTML landmark·실제 anchor로 다시 구현한다.
- 49개 box·84개 text의 원본 밀도를 Hero에 그대로 옮기지 않는다. 5개 layer와 각 layer 최대
  3~5개 node로 압축한다.
- node를 색상만으로 구분하거나 비선택 node를 숨기지 않는다. layer index·border·connector와
  text를 함께 사용하고 모든 정보는 정지 상태에서도 읽을 수 있게 유지한다.
- 물리 bridge의 후보 세부 모드, OVN Local VPC 후보의 지원 완료 주장, multi-node VXLAN 자동화는
  표시하지 않는다.

## 5. 우선순위와 수용 기준

| 우선순위 | 문제·변경 | 수용 기준 |
|---|---|---|
| P0 | 현재 정본 계층 | 세 landing에 5개 layer와 Access 3개, Service 4개, Runtime 5개, Host 4개 node가 있고 금지 기능이 없다. |
| P0 | 계층 포함 관계 | 접근→제어면→서비스→runtime→host의 connector가 한 방향으로 이어지고 layer boundary가 독립적으로 읽힌다. |
| P0 | link·focus | Access 3개와 Service 4개가 유효한 정본 route이며 keyboard focus가 hover와 같은 path signal을 제공한다. |
| P1 | 선택 경로 강조 | 네 service 각각이 대응 runtime·host node를 강조하고 pointer 이탈 시 정지 상태로 복귀한다. |
| P1 | 반응형 | 1440px Hero와 390px 단일 열에서 node label이 겹치거나 page·map overflow가 없다. |
| P1 | motion 안전 | load animation은 없고 reduced motion에서 transform `none`, animation 0.01ms·1회 계약을 유지한다. |
| P1 | 회귀 방지 | 정적 gate가 layer·node 수, 정본 label·link, 금지 label, focus와 reduced-motion 계약을 검사한다. |

## 6. 접근성·반응형·상태 검토

- keyboard/focus: 실제 access/service anchor만 Tab 순서에 들어가며 2px solid focus outline과 대응
  path 강조를 제공한다.
- 색상 외 상태 표현: 선택 node 이동·상단 signal·connector motion·대응 node border를 함께 사용한다.
- loading/empty/error/disabled: 정적 아키텍처와 내부 link이므로 별도 상태가 없다.
- 1024/768/480px: 768px 이하에서 Hero copy 아래로 이동한다. 480px 이하에서는 Access 3열,
  Control·Service·Runtime·Host 2열 또는 마지막 홀수 node full-width로 배치해 세로 길이와 판독성을
  함께 관리한다.

## 7. 정량 검증(선택)

- Attention Insight 사용 여부: 사용하지 않음
- 이유: 사용자가 지정한 구조 기준과 실제 계층·keyboard·motion 정확성이 핵심이고 heatmap이
  containment나 기술 사실을 검증하지 못한다.

## 8. 구현 후 검증

- UI 자동 테스트:
  - Puppeteer에서 `/`, `/ko/`, `/en/` 각각 1440×1000·390×844 검증
  - 각 조합에서 Layer 5, Access link 3, Service link 4, Runtime node 5, Host node 4
  - page·map 가로 overflow 0, node overflow 0, console·page·request error 0
  - axe-core WCAG 2 A/AA violation 0
  - 초기 connector·card animation `none`, storage hover에서 connector
    `pcv-arch-flow-y`·card `pcv-arch-node-signal`, 이탈 뒤 `none` 복귀
  - keyboard focus outline `solid`와 hover 동일 connector flow 확인
  - reduced motion에서 transform `none`, animation duration `0.01ms`, iteration `1`
- bundle freshness와 정적 계약: `cd site && npm run check` 통과, 26 page·86 artifact 및 내부 link,
  layer·node·정본 label·금지 label·motion contract 검증
- 변경 후 캡처/hash:
  - `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-desktop.png`
    — `03be7edc49b5f0b491034fb3f9d7178c0553eaeebd9e840e65fe9c233d0e6867`
  - `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-desktop-map.png`
    — `5d9b164675720d0084222adda90b980076d58421d359257d8fe065db93abecf9`
  - `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-desktop-storage-hover.png`
    — `d04db052bba48f6f6cc608129fce240ddd37dcb873ac70b85eea2cb4fb3c578c`
  - `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-mobile.png`
    — `a6b1e9e861c0aa4470da73e3a1eadb543aee4193214da0c626204fcb98468cf0`
  - `.scratch/ui-reviews/2026-08-24-landing-single-edge-architecture-layers/after-ko-mobile-map.png`
    — `9737fbecbf3a67ecb2a58d91ac32fce0c8517d9de025e17bd902de113b8e2a33`
- 시각 판정: desktop은 Hero copy와 5개 계층 지도가 좌우에서 균형을 이루고, mobile은 계층·node
  label이 겹치지 않으며 선택 경로의 teal connector·대응 runtime·host border가 정지 정보 위에
  보조 신호로 명확히 드러난다.
- 잔여 위험: 로컬 정적 artifact 기준 검증까지 완료했다. 운영 custom domain 반영은 별도
  commit·push·Pages 배포 뒤 확인해야 한다.
