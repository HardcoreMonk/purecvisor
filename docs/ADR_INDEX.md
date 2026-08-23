# ADR 적용 상태 인덱스

> **대상:** `purecvisor-single`
> **현행화 기준:** 2026-08-21
> **목적:** ADR 원문 중 현재 Single Edge 공개 리포에 직접 적용되는 결정과 역사 기록으로만 보존되는 결정을 구분한다.

---

## 1. 읽는 규칙

`docs/adr/`의 ADR은 설계 결정 원문이다. 일부 ADR은 원본 통합 코드베이스 또는 상용 범위 분리 이전의 판단을 포함한다.

현재 Single Edge 작업에서는 다음 우선순위를 따른다.

1. [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md)
2. 이 문서의 적용 상태 표
3. 개별 ADR 원문
4. git 이력과 현재 코드

개별 ADR 원문과 공개판 경계가 충돌하면 공개판 경계를 우선한다.

---

## ✅ 번호 충돌 해소됨 (구 0025·0026 공번 → 0029·0030) — 2026-07-18

M5(v1.3.7) 병합으로 `0025`·`0026`이 각각 두 ADR 파일에 겹쳤던 공번을 **해소 완료**(2026-07-18). 2.0-native 충돌자를 미사용 번호로 이동했다(헤더·소스 주석·문서·파일링크 동반 재번호, 동작 불변 — `make single` 0-error + 테스트/게이트 회귀 0):

| 번호(유지) | 정본 | 재번호된 충돌자 → 이동 후 |
|---|---|---|
| 0025 | `0025-counterfactual-verification-discipline` (반사실 검증 규율) | 구 `0025-rest-ws-tls-always-on` → **`0029-rest-ws-tls-always-on`** |
| 0026 | `0026-host-daemon-mac-hardening-seccomp-nnp-apparmor` (호스트 데몬 MAC) | 구 `0026-sg-overlay-mutual-exclusion` → **`0030-sg-overlay-mutual-exclusion`** |

- **정본 근거(shared-ADR 원칙)**: counterfactual·mac-hardening은 **1.0·2.0 양 라인 공유 ADR**(1.0 `docs/adr/`에 각각 0025·0026으로 존재, 1.0 코드도 ADR-0025=counterfactual 사용) → cross-line 번호 일관을 위해 **번호 유지**. tls-always·sg-overlay는 **2.0-native**(1.0 부재) → 번호 자유 → 이동. 즉 "공유 ADR이 번호를 지키고, 라인-고유 충돌자가 이동".
- **다의성(이력)**: bare `ADR-0025`/`ADR-0026`는 소스에서 다의적이었다(파일별 tls↔counterfactual, sg-overlay↔mac). 재번호는 일괄치환이 아니라 **파일-레벨 분류**로 수행 — tls-always/sg-overlay 지칭만 0029/0030으로 이동, 반사실/MAC 지칭은 0025/0026 유지. CHANGELOG 등 반사실 지칭 다수는 불변.
- **실행 결과**: 충돌자를 미사용 번호로 이동하고 링크와 계약을 갱신했다.

---

## 2. 현재 적용 상태

| ADR | 상태 | Single Edge 적용 |
|-----|------|------------------|
| ADR-0001 | accepted | 활성. 단일 프로세스 + `GMainLoop`, fork 금지 원칙 유지 |
| ADR-0002 | historical | OVN 격리 논의 기록. 현재 Single Edge는 OVN local SDN core를 포함하되 멀티 노드 자동화는 제외 |
| ADR-0003 | historical | DPDK/SR-IOV 격리 논의 기록. 현재 공개판에서는 runtime hardware gate와 테스트 기준으로 판단 |
| ADR-0004 | superseded | ADR-0006에 의해 대체된 기록 |
| ADR-0005 | historical | 회색지대 RPC 사용량 판단 기록. 현재 공개판 범위는 `PUBLIC_RELEASE_BOUNDARY.md`가 우선 |
| ADR-0006 | historical | GPU/iSCSI/Federation 흡수 논의 기록. Single Edge에서는 페더레이션을 공개 기능으로 보지 않음 |
| ADR-0007 | accepted | 활성. security group은 Single Edge 보안 기능으로 유지 |
| ADR-0008 | historical | 고급 네트워크/NFV/iSCSI initiator 판단 기록. 현재 기능 지원 여부는 코드와 UI endpoint registry 기준 |
| ADR-0009 | historical | 회색지대 흡수 판단 기록. 현재 공개 범위 판단은 `PUBLIC_RELEASE_BOUNDARY.md` 기준 |
| ADR-0010 | accepted | 활성. WebSocket 인증은 프로토콜 레벨 auth 메시지 사용 |
| ADR-0011 | historical | 클러스터 I/O fencing 기록. Single Edge 운영 절차로 사용하지 않음 |
| ADR-0012 | accepted | 활성. fire-and-forget 결과 채널 패턴 유지 |
| ADR-0013 | accepted | 활성. 프론트엔드 IIFE 모듈 스코프 유지 |
| ADR-0014 | accepted | 활성. JWT Bearer 기반 REST 인증 유지, CSRF 세션 모델 사용 안 함 |
| ADR-0015 | accepted | 활성. gRPC 비루프백 바인딩 시 TLS 강제 원칙 유지 |
| ADR-0016 | accepted | 활성. Supanova 테마 축소와 accent 변수화 유지 |
| ADR-0017 | accepted | 활성. `vm.delete` 원자성 복구와 XML rollback 유지 |
| ADR-0018 | accepted | 활성. fire-and-forget audit는 워커 콜백에서 기록 |
| ADR-0019 | accepted | 활성. UDS 우회 정책과 메서드명 기반 RBAC 방어 유지. operator의 VM 단일 대상 action은 owner metadata 일치 시만 허용하며, `make check-rbac`가 정책 계약 회귀를 차단 |
| ADR-0020 | accepted | 활성. AI Ops producer → self-healing 호출 체인 유지 |
| ADR-0021 | historical | 분산 ZFS lock 기록. Single Edge는 intra-node 보호와 공개판 경계를 우선 |
| ADR-0022 | accepted | 활성. `vm.create`의 `storage_type` + `storage_pool`/`image_dir` 저장 위치 계약 유지 |
| ADR-0023 | accepted | 활성. `vm.clone`은 source VM owner-scope를 통과한 operator/admin에게 열려 있으며, source VM `shut off` 상태와 준비된 템플릿 확인 또는 `libguestfs-tools`의 `virt-sysprep` + `virt-filesystems` + `guestfish` + `virt-customize` 기반 guest reset을 요구한다. zvol 경로와 qcow2/raw file 경로는 `daemon.conf` 기본값 추정이 아니라 실제 libvirt XML disk source를 기준으로 계산한다. zvol CoW/full clone, qcow2/raw full copy, one-pass MAC 치환, 실패 cleanup을 유지한다. 2026-04-28 CoW clone, 2026-04-29 zvol full clone, Ubuntu 24.04 non-LVM qcow2/raw full clone + guest reset, Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone + guest reset 실환경 검증 완료. Rocky/RHEL LVM, SELinux enforcing boot smoke는 문서상 후속 검증 항목 |
| ADR-0024 | accepted | 활성. Native Host HIDS/HIPS는 Single Edge 호스트 노드 보호로 제한하며, v1은 탐지·감사·알림 중심으로 동작한다. 실행 가능한 HIPS action은 admin 승인 후 `block_ip`와 `revoke_api_key`만 허용하고, baseline은 admin의 명시 refresh 전까지 `unknown`으로 유지한다. |
| ADR-0025 | accepted | 활성. 검증은 반사실을 동반한다 — 통제·게이트·완료는 그 메커니즘을 제거하면 반드시 RED로 드러나야 하며, 효과 테스트는 실 production 코드를 실행한다(replica 금지). |
| ADR-0026 | accepted | 활성. 호스트 데몬 MAC 하드닝 — seccomp/NNP는 LXC 상속·AppArmor 전환 충돌·업계 관행(libvirt/Proxmox)으로 비활성 유지(명시 수용), 실효 통제는 capabilities + AppArmor MAC 프로필(complain 배포→검증 후 enforce opt-in). |
| ADR-0027 | accepted | 활성. NexVisor 경계 — vol2 항목의 purecvisor 2.0 명시 제외 범위 규정. |
| ADR-0028 | accepted | 활성(2.0). 2.0 호스트 데몬 하드닝 = D8 hidepid + capabilities, **AppArmor 데몬-confinement 미배포**(hidepid+AppArmor+libvirtd 3-way 충돌 회피). 2.0에서는 ADR-0026보다 본 ADR 우선. |
| ADR-0029 | verified | 활성. REST/WS TLS 기본 활성 + 자가서명 자동 프로비저닝을 유지한다. nginx 외부 TLS 종료 opt-in은 `PCV-NGINX-TRUST-BOUNDARY: host-loopback`을 만족하는 호스트에서만 사용하고, `PCV-NGINX-COUNTERFACTUAL: untrusted-local-process`인 호스트에는 활성화하지 않는다. **구 0025, 2026-07-18 공번 해소로 재번호**(위 해소 절). |
| ADR-0030 | accepted | 활성. SG × tenant-overlay 상호 배타 — 바인딩 시점 양방향 거부 + 부팅 감사. **구 0026, 2026-07-18 공번 해소로 재번호**(위 해소 절). |
| ADR-0031 | accepted | 활성(2.0). 웹 표준 native `<dialog>` 모달(`PCV.modalCore`)로 모달과 bare overlay를 통합하고, 비모달 알림 센터는 Popover API를 사용한다. |
| ADR-0032 | verified | 활성. 알림 설정은 process-local revision의 compare-and-set, 전체 후보 원자 검증, strict startup/reload source 계약을 사용한다. |
| ADR-0033 | Verified | 활성. 브리지 NIC 게스트 MTU는 attach/정의 시점 브리지 실측 MTU(`/sys/class/net/<bridge>/mtu` 단일 소스)를 정의 XML·`vm.start` 라이브 attach·핫플러그 3경로 모두에 `<mtu size='N'/>`로 1500 포함 항상 명시한다. 대역 밖·읽기 실패는 fail-open(생략+경고). dpdk/SR-IOV는 범위 밖이며 레거시 VM은 재정의 시 수렴한다. |
| ADR-0034 | Verified | 활성. 감사 hashchain append는 원자성과 epoch 연속성을 유지한다. active epoch 파손은 listener 시작 전에 데몬 기동을 중단하고 health/Prometheus에 상태를 노출한다. |
| ADR-0035 | Verified | 활성. `pcvctl`은 0=성공, 1=실행·전송·프로토콜·RPC 실패, 2=사용법·라우팅 오류를 반환한다. |
| ADR-0036 | Verified | 활성. iSCSI initiator CHAP는 비밀 argv 대신 open-iscsi hard-link lock 아래 모든 일치 TPGT·iface node record를 0600 원자 갱신하고 실행 중 실패를 rollback한다. `discoverydb --discover`와 target별 `/var/lib/iscsi/nodes`·legacy `/etc/iscsi/nodes` 선택을 사용하며 양쪽에 같은 target이 있으면 거부한다. |
| ADR-0037 | historical | 공개판 런타임에 영향이 없는 내부 UI 연구 workflow 기록. 공개 저장소의 UI 계약은 `DESIGN.md`, 제품 테스트와 `scripts/check_design_md.py`가 소유한다. |
| ADR-0038 | Verified | 동기 JSON-RPC audit와 Prometheus 결과를 canonical response envelope에서 집계하며 async pending dispatch는 반환 시점 미관측을 오류로 세지 않는다. |
| ADR-0039 | Verified | 제품 Q35 VM에 명시 `pcie-root index=0`과 8개 PCIe root-port를 보장하고 NIC config·MAC·MTU의 재시작 유지와 cleanup을 검증한다. |
| ADR-0040 | Approved | Local VPC는 Linux bridge 우선 backend, VPC 1:N IPv4 subnet, host 전체 비중첩 CIDR과 legacy bridge 비자동 편입 계약을 사용한다. |
| ADR-0041 | Approved | Local VPC는 cross-VPC default deny, attachment anti-spoofing, fail-closed 부팅 quarantine, managed bridge single writer와 제한형 Service Publish를 보안 경계로 사용한다. |
| ADR-0042 | Implemented | 활성. PRIVDROP-1 로컬 후보는 daemon Effective keep-5와 감사된 spawn ceiling을 분리하고, 중앙 spawn의 raw-syscall child setup이 base/storage/signal/DHCP/runtime별 exact capability를 부여한다. LXC 기본 drop 5종은 daemon bounding에서도 제거하고 커널 모듈은 modules-load.d가 선행 준비한다. 안전 C 1344/1344(실 OVS 삭제 1건 skip)+audit 5/5, root 효과·39게이트·정적 반사실은 PASS, main·운영 검증 대기다. |
| ADR-0043 | Implemented | 물리 bridge를 호스트 L3가 없는 전용 Ethernet 업링크로 제한하는 fail-closed guard, rollback 가능한 create·bind·비휘발 desired-state commit, 부팅 reconcile과 우회 차단을 구현·배포했다. shared가 이 계약을 완화하지 않는다. 격리 dedicated NIC와 host reboot 검증 전 `Verified` 승격은 보류한다. |
| ADR-0044 | Implemented | physical bridge를 `uplink_mode=dedicated\|shared`로 분리한다. shared mode는 관리 NIC의 host L3·master·MAC·MTU를 보존하고 게스트 MAC만 upstream 네트워크에 전달한다. 실제 KVM VM·host reboot 검증 전 `Verified` 승격은 보류한다. |
| ADR-0045 | Implemented | Local VPC 생성 시 `linux\|ovn` backend를 고정하고 OVN resource를 external ID 기반 single writer로 수렴한다. 부팅 KVM·Linux/OVN 공존·host/controller reboot·전 단계 fault injection과 공개 지원은 남아 있다. |
| ADR-0046 | Verified | 공개 landing과 Pages hosting은 Astro·Starlight를 사용하고 `/`·`/ko/`는 한국어, `/en/`은 영어를 제공한다. 2026-08-23 Pages run `32645170384`와 custom domain에서 검증했다. 공개 reader·navigation 계약은 ADR-0047이 승계한다. |
| ADR-0047 | Implemented | 공개 운영 가이드는 `docs/GUIDE.md`의 22개 장을 `/ko/<분류>/<문서>/` 정적 page로 생성한다. 기본 진입은 `/ko/getting-started/installation/`이며 landing·Header는 새 route를 사용하고 `/docs.html`은 legacy hash redirect만 유지한다. Pages 배포 검증 전이다. |

---

## 3. 새 ADR 작성 기준

새 ADR은 다음 조건 중 하나에 해당할 때 추가한다.

- 공개판 경계를 바꾸는 결정
- 장기 유지할 아키텍처 원칙 변경
- 보안 모델, RBAC, audit, async 결과 채널 변경
- 운영자가 실환경에서 따라야 하는 절차 변경
- 기존 ADR을 폐기하거나 대체해야 하는 경우

새 ADR에는 반드시 `Single Edge 적용 상태`를 명시한다.
새 ADR 상태명은 lifecycle contract의 `Draft -> Review -> Approved -> Implemented -> Verified -> Archived`를 사용한다.
기존 ADR의 `Accepted`는 `Approved`의 legacy alias로 해석한다.

---

## 4. 공개판 경계와의 관계

Single Edge 공개판에서 기능 절차를 문서화할 수 있는 조건:

- 현재 리포에서 빌드되는 코드가 있다.
- `make single` 산출물에서 접근 가능한 표면이다.
- Web UI, CLI, REST, UDS 중 하나 이상의 검증 가능한 진입점이 있다.
- 공개판 금지 표식에 해당하지 않는다.

위 조건을 만족하지 않는 내용은 운영 가이드가 아니라 역사 기록 또는 범위 밖 설명으로만 남긴다.
