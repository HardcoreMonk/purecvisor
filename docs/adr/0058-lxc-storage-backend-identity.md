# ADR-0058: LXC 저장소 backend와 실제 identity 고정

- 상태: Verified
- 날짜: 2026-09-16
- 승인 근거: native LXC/Btrfs 검증 후 사용자의 다음 단계 구현 승인

## 맥락

기존 컨테이너 driver는 ZFS dataset 이름을 현재 기본 pool에서 다시 계산한다. Btrfs를 추가하면서 같은 규칙을 유지하면 설정 변경 시 다른 저장소에 mutation을 전달할 수 있다. Native LXC 복제·복원은 PureCVisor owner/image sidecar를 보존하지 않는다.

## 결정

기본값은 ZFS로 유지하고 `[container] storage_backend=btrfs`를 명시적으로 선택한다. 컨테이너마다 rootfs 밖의 `purecvisor.storage`에 실제 backend와 저장소 identity를 기록한다. 이후 mutation은 기록과 실제 filesystem/subvolume을 대조한다. legacy ZFS만 실제 mountpoint 확인을 통해 호환하며 다른 native 컨테이너의 자동 편입은 하지 않는다.

Btrfs clone/snapshot/restore는 정지 상태에서 수행한다. snapshot은 rootfs만 포함하며 nested subvolume과 외부 mount는 최초 지원에서 거부한다. 복원은 현재 config/owner/meta를 보존하고 journal과 원자 교환으로 rootfs를 교체한다. 삭제는 검증된 관리 대상만 처리하고 부분 실패를 재시도 가능 상태로 남긴다.

## 대안과 영향

- 배포판 자동 선택: 잘못된 mount와 예측하기 어려운 fallback 때문에 기각.
- native same-name snapshot restore: owner/meta 소실 때문에 기각.
- parent snapshot: 자식 rootfs subvolume이 포함되지 않으므로 기각.
- 전체 저장소 추상화/VM 변경: 현재 Single Edge LXC 목적을 넘으므로 제외.

rootless Btrfs, quota, nested subvolume 재귀 snapshot, 자동 import와 backend 간 migration은 지원 범위가 아니다. 기존 ZFS 동작과 owner/RBAC/비동기 audit 계약을 보존한다. 구현·실기 상태는 [계획](../superpowers/plans/2026-09-16-lxc-btrfs.md)과 운영 인계에서 별도로 추적한다.

## 검증 근거

[2026-09-16 API 검증 인계](../operations/2026-09-16-lxc-btrfs-api-validation.md)의 지정 Arch/Btrfs 호스트에서 생성·부팅·복제·rootfs 복원·실패 보존·journal 복구·삭제와 Job/WS/audit 결과를 대조했다. 격리 회귀와 실제 검증을 구분하며, 장시간·정전·전체 Ubuntu ZFS 회귀로 확대하지 않는다.
