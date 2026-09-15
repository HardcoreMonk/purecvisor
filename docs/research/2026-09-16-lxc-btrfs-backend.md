# LXC Btrfs 백엔드 조사

> 조사일: 2026-09-16 KST
> 상태: 조사 완료 · 구현 전 제안
> 조사 기준: PureCVisor 공개 소스 `5e84387`, LXC 공식 매뉴얼, Btrfs 공식 문서와 시험 호스트의 읽기 전용 점검
> 현재 제품 계약: PureCVisor 2.0.0의 LXC 생성에는 ZFS가 필요하다. 아래 후보 설정과 명령은 현재 PureCVisor의 지원 기능을 의미하지 않는다.

## 1. 결론과 배포판 전제

**Btrfs는 PureCVisor의 선택형 컨테이너 저장 백엔드로 검토할 수 있다.** LXC는 Btrfs 서브볼륨 생성과 CoW 복제를 지원한다. PureCVisor에는 ZFS를 직접 호출하는 경로가 남아 있어 저장소별 동작을 분리해야 한다.

Arch의 파일시스템은 설치자가 선택한다. [Arch 설치 가이드](https://wiki.archlinux.org/title/Installation_guide#Format_the_partitions)는 적절한 파일시스템 선택과 Ext4 예제를 제공한다. 모든 Arch 설치가 Btrfs라는 전제는 사용하지 않는다. [Omarchy 공식 수동 설치 안내](https://learn.omacom.io/2/the-omarchy-manual/96/manual-installation)는 Btrfs와 기본 서브볼륨 구조·압축을 지정한다.

시험한 Omarchy 호스트의 현재 읽기 전용 점검 결과:

| 항목 | 관찰 |
|---|---|
| 루트·`/var/lib`·`/var/lib/purecvisor`·`/var/lib/lxc` | Btrfs, `subvol=/@`, `compress=zstd:3` |
| 설치 패키지 | `btrfs-progs 7.1-1`, `lxc 1:7.0.0-2` |
| 다운로드 템플릿 | `/usr/share/lxc/templates/lxc-download` 존재 |
| 커널 점검 | namespaces와 cgroup v2의 주요 컨트롤러 활성 |
| 별도 확인 사항 | `lxc-checkconfig`가 `newuidmap`·`newgidmap`의 setuid-root 부재를 경고함. 일반 사용자 실행과 데몬이 만드는 user namespace 컨테이너를 별도로 검증해야 함 |
| PureCVisor 기본 LXC 경로 | `/var/lib/purecvisor/lxc`는 아직 없음. 상위 경로의 Btrfs만 확인 |
| 실기 범위 | 컨테이너·서브볼륨 생성, quota 변경, 패키지 설치, 서비스 재시작은 수행하지 않음 |

이 결과는 시험 준비 가능성을 뒷받침한다. PureCVisor를 통한 Btrfs 컨테이너 부팅 성공이나 Arch 전체 지원 인증은 아니다.

## 2. 공식 지원 기능

| 동작 | 공식 근거와 의미 |
|---|---|
| 생성 | [`lxc-create -B btrfs`](https://man.archlinux.org/man/lxc-create.1.en)는 대상 경로가 Btrfs일 때 rootfs를 새 서브볼륨으로 만든다. `-P`로 관리 경로를 지정할 수 있다. |
| 복제 | [`lxc-copy`](https://man.archlinux.org/man/lxc-copy.1.en)는 Btrfs를 지원한다. `-s`가 스냅샷 방식이며 전체 복제와 구분해야 한다. 실행 중 복제는 별도 정합성 조건이 필요하다. |
| 스냅샷과 복원 | [`lxc-snapshot`](https://man.archlinux.org/man/lxc-snapshot.1.en)은 `snap0` 같은 이름을 자동 부여한다. 같은 이름으로 복원하면 원본 컨테이너를 삭제하고 교체하므로 PureCVisor의 사용자 지정 이름·복구 계약과 그대로 같지 않다. |
| Btrfs 자체 동작 | [서브볼륨 명령](https://btrfs.readthedocs.io/en/latest/btrfs-subvolume.html)은 생성·스냅샷·목록·삭제를 제공한다. 서브볼륨 삭제의 경로 제거와 실제 공간 회수는 구분된다. |

LXC upstream 소스는 조사 시점의 [`4f1258197c159b2d2d4bfcffb881872526560b08`](https://github.com/lxc/lxc/blob/4f1258197c159b2d2d4bfcffb881872526560b08/src/lxc/storage/btrfs.c)를 참고했다. 이 코드는 설치된 Arch 패키지의 동일 소스라고 가정하지 않는다. `btrfs_create_clone()`의 데이터 복사, `btrfs_create_snapshot()`의 Btrfs ioctl, `btrfs_destroy()`의 서브볼륨 정리 경로를 확인했다. 구현 전에 실제 설치 패키지의 소스·패치와 다시 대조해야 한다.

## 3. PureCVisor에서 바꿔야 하는 경계

소스 경로는 이 보고서와 같은 공개 저장소를 기준으로 한다.

| 위치 | 현재 동작 | Btrfs 추가 시 요구사항 |
|---|---|---|
| [`lxc_driver.c`](../../src/modules/lxc/lxc_driver.c), `_lxc_create_thread` | ZFS 부모 데이터셋 확인·생성 → 컨테이너 데이터셋 생성 → `lxc-create` | 사전 검사·생성·실패 정리를 백엔드에 위임. Btrfs 경로에서 ZFS 실행 금지 |
| 같은 파일, `_lxc_destroy_thread` | `lxc-destroy` 이후 `zfs destroy -r`, 일반 디렉터리 재귀 정리 | 소유한 서브볼륨과 스냅샷만 정리하고 실제 잔여 상태로 성공 판정. 현행 ZFS 정리 오류의 경고 처리와 일반 재귀 삭제를 Btrfs에 그대로 적용하지 않음 |
| 같은 파일, `_clone_worker` | `lxc-copy -B zfs` 고정 | 원본의 저장 백엔드·파일시스템을 확인하고 전체 복제/CoW 복제 의미를 명시 |
| 같은 파일, 스냅샷 4개 작업 | `zfs snapshot`, `rollback -r`, `destroy`, `list` 직접 호출 | 이름 매핑, 설정 보존 범위, 정지 조건, 안전한 복원과 부분 실패 복구 정의 |
| 같은 파일, 목록·IP 조회 | `zfs mount -a` 또는 개별 데이터셋 마운트 | 관리 중인 저장소만 확인. Btrfs에서 전역 ZFS 마운트 시도 방지 |
| [`handler_container.c`](../../src/modules/dispatcher/handler_container.c), `_ensure_container_config_ready` | 설정이 없으면 ZFS 데이터셋 마운트 | 파일시스템·저장소 식별자에 맞는 준비 검사로 분리 |
| [`pcv_config.c`](../../src/utils/pcv_config.c) | `container_pool`은 ZFS 경로, `lxc_path`는 파일 경로 | 두 의미를 유지하고 새 백엔드 선택을 별도 설정으로 정의 |
| [`lxc_owner.c`](../../src/modules/lxc/lxc_owner.c)와 `purecvisor.meta` | 컨테이너 디렉터리에 소유자·이미지 메타데이터 보존 | backend, rootfs 식별자와 복구 메타데이터를 영속화. clone·rollback 시 RBAC 소유권이 되돌아가지 않게 검증 |
| RPC·REST·CLI·설치·도움말 | 백엔드 선택·기능별 지원 상태 계약 없음 | 선택·오류·지원 가능한 작업을 일관되게 전달하고 실제 결과 audit/비동기 완료 통지를 유지 |

`lxc-create`에 `-B btrfs`만 붙이는 수정은 이 경계를 충족하지 못한다. 이미 존재하는 ZFS 컨테이너는 설정 기본값을 바꾼 뒤에도 원래 저장소로 처리되어야 한다.

## 4. 접근법 비교

| 후보 | 장점 | 비용·제약 | 평가 |
|---|---|---|---|
| **컨테이너 저장 계층을 분리하고 ZFS + Btrfs 지원** | 기존 ZFS 경로를 보존하면서 생성부터 복구까지 같은 인터페이스로 검사 가능 | 저장소 식별자·작업별 정리·복구 계약과 회귀 검증 필요 | **추천** |
| 모든 저장 작업을 LXC 네이티브 API/CLI로 통일 | upstream 구현 재사용 범위가 큼 | 기존 ZFS 데이터셋 배치, 사용자 스냅샷 이름, 복원·owner 계약의 변경 폭이 큼 | 후속 비교 후보 |
| 일반 디렉터리 백엔드부터 추가 | 파일시스템 제약이 적고 기본 생성 경로가 단순함 | CoW 스냅샷·복제 요구를 별도로 풀어야 하며 Btrfs의 장점을 사용하지 못함 | 이번 목적의 우선안 아님 |

## 5. 추천안의 설계 후보

아래 항목은 구현 전에 검토할 제안이며 승인된 ADR 또는 실제 설정이 아니다.

1. **명시적 선택:** 후보 설정 `[container] storage_backend=zfs|btrfs`. 기존 기본값은 ZFS로 유지한다. 배포판 이름으로 선택하거나 실패 시 다른 저장 방식으로 자동 전환하지 않는다.
2. **객체별 식별:** 생성 때 backend, 관리 root, Btrfs filesystem UUID·subvolume ID 또는 ZFS dataset을 기록한다. 누락된 기존 객체는 실제 저장소를 대조하고, 모호한 상태에서는 삭제·복원을 거부한다.
3. **저장 단위:** Btrfs rootfs는 LXC 네이티브 서브볼륨 경로를 우선 검증한다. LXC config와 `purecvisor.owner`·`purecvisor.meta`는 rootfs 밖에 있으므로 별도 원자적 기록과 복구 정책이 필요하다. OS 스냅샷 복원 때 설정만 되돌아가고 rootfs는 남는 상태도 시험한다.
4. **초기 지원 범위:** 생성·조회·시작·정지·삭제와 정지된 컨테이너의 스냅샷·복제·복원을 한 지원 단위로 검증한다. 설정·네트워크·소유권 중 무엇을 복원하는지 먼저 정의한다. 권한 정보는 과거 snapshot의 값을 무조건 덮어쓰지 않는다.
5. **복원 절차:** 원본 보존 → 후보 rootfs 준비 → 설정·파일 검증 → 전환 → 결과 확인 → 이전 rootfs 정리 순서와 crash recovery 기록을 설계한다. 성공 경로만 있는 rename 두 번으로 원자성을 주장하지 않는다.
6. **초기 제외 기능:** 다른 파일시스템으로의 CoW 복제, ZFS↔Btrfs 자동 변환, 실행 중 애플리케이션 정합 스냅샷, Btrfs send/receive 기반 제품 백업과 저장 용량 quota는 별도 계약·검증 전까지 지원 상태로 표시하지 않는다.

새 ADR 후보는 **컨테이너 저장 백엔드 선택·객체별 영속 식별·스냅샷 복원 경계**다. 기존 ADR-0001의 프로세스 모델과 ADR-0012/0018의 비동기 결과·audit 정책을 유지한다. ZFS VM 생성·clone ADR의 저장 계약을 컨테이너 Btrfs 계약으로 확장했다고 가정하지 않는다.

## 6. 검증에서 놓치면 안 되는 차이

- **중첩 서브볼륨:** [Btrfs 문서](https://btrfs.readthedocs.io/en/latest/Subvolumes.html#nested-subvolumes)에 따르면 스냅샷은 재귀적이지 않다. guest 안의 하위 서브볼륨과 bind mount 데이터의 제외 범위를 탐지·안내하고, 포함을 약속한 데이터가 빠지면 작업을 거부해야 한다.
- **동일 파일시스템:** CoW 복제의 원본·대상 filesystem UUID를 비교한다. 다른 Btrfs 파일시스템으로의 이동을 경로 문자열만으로 허용하지 않는다.
- **공간과 quota:** [qgroup 문서](https://btrfs.readthedocs.io/en/latest/btrfs-qgroup.html)는 공유/독점 사용량과 quota 일관성 조건을 구분한다. quota가 비일관 상태이면 제한이 작동하지 않을 수 있다. 컨테이너별 용량 제한을 단순한 디렉터리 크기 검사로 대체하지 않는다.
- **삭제:** 스냅샷·서브볼륨 경로 제거 이후 공간 회수가 지연될 수 있다. 관리 경계 밖 데이터, symlink와 외부 mount에 재귀 삭제가 넘어가지 않도록 한다.
- **백업:** [Btrfs send](https://btrfs.readthedocs.io/en/latest/btrfs-send.html)는 read-only 스냅샷을 전제로 한다. 기존 ZFS send/receive 백업 코드를 그대로 사용할 수 없다.
- **user namespace:** 현재 코드의 `rootless` 설정은 데몬 실행 사용자와 구분해서 검증한다. subordinate UID/GID, rootfs 소유권, 하위 프로세스 capabilities, AppArmor·seccomp 조건이 별도다. Btrfs 사용 자체가 rootless 성공을 보장하지 않는다.

## 7. 다음 검증 단계 제안

1. **격리된 LXC 실기:** 시험 전용 Btrfs 저장 경로에서 LXC 자체의 생성·부팅·파일 영속화·정지·CoW 복제·스냅샷 복원·삭제를 확인한다. 시험 자원과 기존 호스트의 설정·서브볼륨은 구분한다.
2. **설계 확정:** 저장 단위, 스냅샷 이름, 복원 대상 설정, owner 보존과 장애 복구를 ADR/spec에서 검토하고 구현 계획을 작성한다.
3. **PureCVisor 통합:** API 접수 응답, 실제 LXC 상태, 작업 결과 audit와 완료 통지를 함께 검사한다. ZFS 미설치 Btrfs 노드에서 모든 지원 경로가 ZFS를 호출하지 않는지 검증한다.
4. **실패·재시작:** 비-Btrfs 경로, read-only/ENOSPC, 다운로드 중단, mount 누락, 중첩 서브볼륨, 동시 요청, 복원 전환 실패, 데몬 재시작·호스트 재부팅을 시험한다.
5. **기존 ZFS 회귀:** 생성·복제·스냅샷·rollback·삭제와 기존 객체의 기본값 변경 후 처리를 Ubuntu ZFS 환경에서 재검증한다.

이번 조사에서는 1~5단계의 실기나 백엔드 구현을 실행하지 않았다. 공식 지원과 소스 구조를 확인했으며, 지원 판정은 위 검증 이후에 내린다.
