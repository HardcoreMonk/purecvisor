# ADR 적용 상태 인덱스

> **대상:** `purecvisor-single`
> **현행화 기준:** 2026-05-04
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
