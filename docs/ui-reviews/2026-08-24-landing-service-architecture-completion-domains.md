# 공개 랜딩 서비스 아키텍처 완료·도메인 보강 UI 리뷰

> **일자:** 2026-08-24
> **판정:** PASS
> **대상:** `/`, `/ko/`, `/en/` Hero의 Single Edge 서비스 아키텍처
> **선행 리뷰:** `2026-08-24-landing-architecture-route-explorer.md`

## 1. 제품 맥락과 목표

- 사용자: 서비스 요청이 단일 노드에서 어떻게 처리되고 완료되는지 확인하려는 운영자와 기술
  의사 결정자
- 핵심 작업: Access에서 transport·policy gate를 거쳐 서비스 domain, 로컬 상태, host integration과
  Linux/KVM 자원까지 이어지는 경로를 읽고 대응 운영 가이드로 이동한다.
- 기억해야 할 두 사실: `purecvisorsd`는 C23·GMainLoop 단일 프로세스이며 비동기 accepted 응답은
  실제 성공이 아니다.
- 제약: Single Edge 공개 경계, Astro·Starlight, code-native HTML·CSS·SVG, 한국어·영어,
  light·dark와 keyboard·reduced-motion 지원을 유지한다.

## 2. 입력 자료 분석

압축 자료는 전체·제어면·도메인 3종마다 Mermaid, SVG와 PNG를 제공하고 전체 설명 문서와 렌더
설정을 포함한다. 전체 PNG는 2400×3622px, 제어면 PNG는 2000×3093px으로 landing 안에서 그대로
축소하면 읽기 어렵다.

| 발견 | 적용 |
|---|---|
| Access가 Web UI·REST·pcvctl·gRPC·Prometheus 다섯 소비자를 구분 | 01 Access에 다섯 진입점을 유지하고 실제 guide가 있는 세 항목만 link로 제공 |
| daemon TLS 기본과 nginx host-loopback opt-in이 별도 경계 | Control plane 상단에 두 TLS 경계를 병렬 표시 |
| UDS·REST·gRPC·WebSocket은 별도 서비스가 아닌 단일 daemon transport | `purecvisorsd` 내부 Transport stage로 배치 |
| dispatcher가 policy·RBAC·owner scope와 O(1) handler lookup을 수행 | Dispatch gate를 transport와 domain handler 사이에 배치 |
| 동기는 canonical response, 비동기는 accepted Job 이후 worker callback에서 완료 | ADR-0038 sync lane과 ADR-0018 async lane을 분리하고 `accepted ≠ success` 명시 |
| Workload·Network·Storage·Security·Operations 다섯 domain | 기존 네 path를 다섯 domain control과 guide link로 교체 |
| domain은 SQLite WAL·desired state와 host platform 양쪽에 연결 | 04를 State & Adapters로 바꾸고 두 하위 그룹으로 분리 |
| SVG가 `DejaVu Sans`에 고정되고 PNG는 한국어 glyph를 안정적으로 보장하지 못함 | 외부 렌더 이미지를 삽입하지 않고 Pretendard 기반 code-native figure 유지 |

자료 문서가 언급한 `.ua/knowledge-graph.json`은 공개 stage에 없으므로 해당 수치는 재사용하지
않았다. 표시 내용은 `docs/GUIDE.md`, `docs/DATABASE_STRUCTURE.md`, ADR-0001·0018·0029·0038과
실제 `src/` 경로로 다시 대조했다.

## 3. Refero 연구와 reference lock

| 역할 | 출처 | ID | 채택 규칙 |
|---|---|---|---|
| Primary | Tailscale | `5d884659-1d6b-4b82-8ccd-dbb0434667a8` | 차분한 infrastructure 문서, 얇은 border, sans 본문과 mono 기술 label 분리 |
| Secondary | PlanetScale | `c0f79217-5105-4765-bf7a-8ccc9a3284c4` | 낮은 대비 blueprint grid와 날카로운 정보 계층 |
| Diagram structure | Relume sitemap | `cf15c67b-71cf-4a9b-aa37-d021f95e65d9` | 얇은 직교 connector, 넉넉한 여백, 단순한 node hierarchy |
| Selected path | Eraser | `66b9ff99-f7f2-480c-97e9-42718e49cb97` | dark surface에서 색을 선택 edge와 기술 label에만 제한 |

Reference lock:

- Primary direction: Tailscale의 차분한 정밀도와 PureCVisor의 light/dark surface
- Preserve: 5계층, 32px figure, 16px selected route, 12px 최소 label, 5개 선택 domain,
  keyboard·reduced-motion
- Borrow only: PlanetScale의 blueprint grid, Relume의 얇은 연결선, Eraser의 제한된 path accent
- Reject: 외부 diagram 이미지, 전체 mono 한국어, 대면적 neon, 장식 gradient, 큰 radius 반복

## 4. 결정 ledger

| 결정 | 근거 | 적용 이유 |
|---|---|---|
| 5개 세로 계층은 유지하고 Control plane 내부를 stage로 세분 | 기존 학습 비용, 입력 자료 Part 1 | landing 정보 구조를 깨지 않고 실제 요청 처리 순서를 복원 |
| sync와 async를 두 lane으로 분리 | ADR-0018·0038 | accepted를 성공으로 오해하는 가장 큰 운영 위험을 시각적으로 차단 |
| capability path를 서비스 domain 다섯 개로 재분류 | 입력 자료 Part 2, 공개 가이드 정보 구조 | 네트워크 구현 방식 둘을 합치고 Security·Operations 누락을 보완 |
| 04를 State & Adapters 두 그룹으로 분할 | persistence와 host integration의 다른 책임 | SQLite/desired state와 Linux integration을 같은 runtime으로 오인하지 않게 함 |
| 한국어 node에 sans, stage·index에 mono 사용 | SVG font 고정 문제, Tailscale | glyph 안정성과 기술 diagram의 정밀한 인상을 함께 확보 |
| mobile에서 2열 domain과 마지막 전체 폭 항목 사용 | 5개 항목의 홀수 구조 | 이름을 읽을 폭을 보존하면서 과도한 세로 길이를 억제 |

## 5. 수용 기준

| 우선순위 | 기준 |
|---|---|
| P0 | 세 landing에 5계층, 3개 Access link, 5개 domain guide link와 5개 path control이 있다. |
| P0 | sync canonical response와 async accepted·worker completion이 별도 lane으로 표시된다. |
| P0 | 다섯 domain 선택이 label, state·adapter와 host node를 함께 바꾼다. |
| P0 | 1440·1024·768·390px의 light·dark에서 가로 overflow, 겹침, 잘림과 console error가 없다. |
| P1 | 한국어 glyph가 tofu 없이 표시되고 기술 label만 mono를 사용한다. |
| P1 | keyboard roving tabindex, live status, Home·End·arrow key와 reduced motion이 유지된다. |
| P1 | 외부 PNG·SVG runtime dependency와 Multi Edge 범위가 없다. |

## 6. 구현 후 검증

- `cd site && npm run check`: PASS, 26 pages와 86 files artifact 검증
- `make single`, `make test`: PASS, C23 Single Edge build와 1,370개 test runner·audit startup
  test 통과
- `make check-all`: PASS, 38개 공개 계약 gate 통과
- `make release`: PASS, release build 경고 0
- `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`: PASS
- `python3 scripts/check_design_md.py`, `python3 scripts/strip_source_comments.py --check`,
  `make check-public-comments`: PASS
- 실브라우저 24개 조합: `/`, `/ko/`, `/en/` × light/dark × 1440/1024/768/390px
- Axe WCAG A/AA violation 0, console·page·request error 0, page·map·route·control·node 가로
  overflow 최대 0px, figure 밖 node 0
- map 높이: 1440px 1,535px, 1024px 1,693px, 768px 1,982px, 390px 한국어
  2,068px·영어 2,099px
- 계산 style: outer radius 32px, selected route radius 16px, offset shadow 6px/6px/0,
  본문 Pretendard·index/stage mono와 최소 label 12px
- 다섯 domain 모두 pointer 선택, 단일 `aria-pressed`·tab stop, live label, state·adapter·host
  mapping 통과
- keyboard 순환 `workloads → network → operations → workloads → operations`, reduced motion 통과
- light·dark desktop·mobile 캡처를 직접 검사해 한국어 tofu, 겹침과 잘림이 없음을 확인

최종 캡처 SHA-256:

| 캡처 | SHA-256 |
|---|---|
| Korean light desktop | `8b53915b1c97414d91ff246db4a869b22ec54d7645b18121fa31ed4ac5415b66` |
| Korean dark desktop | `7eaff697efc287ebd8c95259a401117de95149dc896b7af11befbcc122c0672e` |
| Korean light mobile | `2aceea602950d45c30d4e24ed8186a5622508fc5693fef79fd282d53e84347b4` |
| Korean dark mobile | `1fb2799bca25cbf79b30eed2520af60584309bf2d6727af107138d9d81bafd30` |

P0·P1 수용 기준을 모두 충족했으며 열린 P2는 없다.
