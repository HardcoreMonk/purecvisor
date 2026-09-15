# LXC Btrfs 실기 검증 인계

> 검증일: 2026-09-16 KST
> 승인 범위: [Btrfs 조사](../research/2026-09-16-lxc-btrfs-backend.md)의 다음 단계인 격리된 LXC 실기
> 판정: LXC 자체의 지정 Btrfs 동작 검증 완료 · PureCVisor 백엔드 구현 전

## 1. 결과 요약

**Omarchy의 Btrfs 위에서 LXC 컨테이너 생성·부팅·복제·스냅샷 복원이 동작했다.**

최종 실행은 정상·거부 조건 8개, 저장소 한계 재현 2개와 정리 1개를 포함한 11개 항목을 충족했다. 시험 후 호스트 보존 검사 14개도 통과했다. 이 결과는 LXC 네이티브 CLI 경로의 실증이며 PureCVisor API를 통한 Btrfs 지원 완료가 아니다.

PureCVisor 2.0.0의 컨테이너 생성에 ZFS가 필요하다는 현재 가이드 계약은 계속 적용한다.

## 2. 시험 환경과 격리

| 항목 | 실제 값 |
|---|---|
| 호스트 | Omarchy 4.0.3, Arch 계열, x86_64 |
| 커널 | `7.2.3-arch1-3` |
| 패키지 | `lxc 1:7.0.0-2`, `btrfs-progs 7.1-1` |
| 파일시스템 | Btrfs, 루트 서브볼륨 `@`, `compress=zstd:3` |
| 게스트 | Ubuntu 24.04.5 LTS, PID 1 `systemd` |
| 이미지 | LXC 기본 서버의 `ubuntu/noble/amd64/default/20260913_07:42` |
| 자원 제한 | 게스트 안에서 `memory.max=536870912` 확인 |
| 네트워크 | `lxc.net.0.type=empty`, 게스트의 인터페이스는 `lo`만 존재 |
| 권한 | root가 관리하는 privileged LXC. 일반 사용자 실행·user namespace 컨테이너는 이번 범위 밖 |
| 관리 경로 | 무작위 식별자가 붙은 시험 전용 Btrfs 서브볼륨과 명시적 `-P` 경로 |
| 다운로드 캐시 | 시험 증거 디렉터리 아래 전용 `LXC_CACHE_PATH`, 해시 기록 후 제거 |

기존 LXC 설정, PureCVisor 설정·데몬, 패키지, 네트워크와 ZFS 구성을 변경하지 않았다. 호스트나 PureCVisor 서비스를 재시작하지 않았다.

## 3. 최종 검증 항목

| 번호 | 항목 | 확인 내용 |
|---|---|---|
| 1 | Btrfs rootfs 생성 | `lxc-create -B btrfs -t download` 성공, rootfs inode 256과 서브볼륨 정보 확인 |
| 2 | 실제 게스트 부팅 | `RUNNING`, guest attach, Ubuntu OS 식별, PID 1 systemd, 메모리 제한·루프백만 존재 확인 |
| 3 | 파일 영속화 | 게스트에서 파일 기록·sync → 정지 → 시작 후 동일 내용 확인 |
| 4 | CoW 복제 | 정지 상태에서 `lxc-copy -B btrfs -s`; 원본과 다른 UUID·원본을 가리키는 Parent UUID 확인. clone 부팅·파일 수정 뒤 원본 불변 |
| 5 | 스냅샷 생성·조회 | `lxc-snapshot`의 `snap0`, snapshot rootfs와 저장 파일 확인 |
| 6 | 같은 이름 복원 | snapshot 이후 원본 파일 변경 → 정지 → `snap0` 복원 → 재부팅 후 이전 파일 복구. 독립 clone의 수정 내용 보존 |
| 7 | 중복 생성 거부 | 이미 정의된 이름은 종료 코드 1로 거부, 원본 파일 SHA-256 불변 |
| 8 | 비-Btrfs 경로 거부 | tmpfs 시험 경로에서 실제 rootfs 생성 시 종료 코드 1과 Btrfs ioctl 오류로 거부 |
| 9 | 중첩 서브볼륨 한계 | 원본 rootfs 안에 별도 서브볼륨·파일을 만든 뒤 CoW 복제. 원본 파일은 남고 clone에서는 해당 파일이 빠짐을 재현 |
| 10 | 상위 스냅샷 한계 | 시험 관리 루트의 read-only 스냅샷에 LXC config는 존재하지만 하위 rootfs 서브볼륨의 `/etc/os-release`는 빠짐을 재현 |
| 11 | 시험 자원 정리 | 컨테이너·snapshot·시험 루트·tmpfs 시험 디렉터리 제거, 잔여 서브볼륨과 cgroup 없음 |

9·10번은 전체 데이터 보존 성공 항목이 아니다. [Btrfs의 비재귀 스냅샷 특성](https://btrfs.readthedocs.io/en/latest/Subvolumes.html#nested-subvolumes)을 실제로 재현한 한계 검사다.

## 4. PureCVisor 통합에 필요한 추가 처리

### 4.1 소유자와 이미지 메타데이터

원본 컨테이너의 rootfs 밖에 시험용 `purecvisor.owner`와 `purecvisor.meta`를 배치한 뒤 네이티브 동작을 관찰했다.

| 시점 | `purecvisor.owner` | `purecvisor.meta` |
|---|---|---|
| 원본 생성 후 | 존재 | 존재 |
| `lxc-copy`로 만든 clone | 없음 | 없음 |
| 원본의 같은 이름 복원 직전 | 최신 시험 소유자 값 존재 | 존재 |
| `lxc-snapshot -r snap0` 이후 | 없음 | 없음 |

따라서 LXC 명령의 성공만으로 PureCVisor 객체 복구 성공을 판정할 수 없다. clone의 소유권 부여와 이미지 메타데이터 기록, rollback의 현재 권한 보존, 파일 기록 실패 시 복구 정책이 필요하다. [LXC 스냅샷 매뉴얼](https://man.archlinux.org/man/lxc-snapshot.1.en)은 같은 이름 복원이 기존 컨테이너를 교체하는 동작임을 설명한다.

### 4.2 저장 단위와 복원 범위

rootfs, LXC config와 PureCVisor 메타데이터를 하나의 사용자 객체로 식별해야 한다. 관리 디렉터리의 상위 스냅샷만으로 이 객체 전체가 보존된다고 가정할 수 없다. 게스트가 만든 하위 서브볼륨과 bind mount의 포함·제외 범위를 탐지하고, 지원하지 않는 조합은 데이터 작업 전에 거부해야 한다.

### 4.3 삭제와 실패 판정

기존 PureCVisor의 일반 디렉터리 재귀 정리를 Btrfs 삭제 경로에 그대로 적용하지 않는다. 관리 대상의 filesystem/subvolume 식별과 snapshot 관계를 대조하고, 잔여 상태를 확인한 뒤 실제 결과 audit과 완료 통지를 보낸다. 이번 시험의 경로 제거 완료는 Btrfs의 모든 공간 회수가 즉시 완료된다는 보장은 아니다.

## 5. 초기 검사 오류와 시정

첫 실행은 7번까지 통과한 뒤 8번의 시험 단언에서 중단됐으며, 시험 자원은 정상 정리됐다. 초기 거부 시험에 사용한 `-t none`은 rootfs 생성을 생략하므로 tmpfs에서도 컨테이너 정의만 생성할 수 있었다. [LXC 생성 매뉴얼](https://man.archlinux.org/man/lxc-create.1.en)의 해당 의미를 다시 대조하고, 8번을 실제 다운로드 템플릿의 rootfs 생성 경로로 수정해 전체 검증을 다시 통과했다. 제품 소스 결함으로 분류하지 않았다.

호스트 사후 검사의 원시 `btrfs subvolume list` 문자열 대조도 처음에는 달랐다. 차이는 기존 `@`, `@home`, `@log`의 generation 증가뿐이었다. 파일 쓰기에 따라 바뀌는 generation을 분리하고 ID·부모 ID·경로의 동일성과 시험 객체 부재를 확인했다. 원본 비교 결과도 증거에 보존했다.

## 6. 호스트 보존과 증거

- PureCVisor `active`, 동일 PID·재시작 횟수, HTTPS health 200과 `status=ok` 확인.
- 데몬·설정·기존 LXC 설정 파일 SHA-256 동일.
- 기존 VM·LXC 목록, 패키지 버전, 네트워크 주소·route 동일.
- 기존 서브볼륨 ID·부모 ID·경로 동일. 시험 서브볼륨·cgroup·캐시 없음.
- 원시 명령 로그, 첫 실행과 최종 실행, before/after, runner, 이미지 캐시 파일 해시를 비공개 증거 묶음으로 보존했다. 수신 후 내부 파일 SHA-256도 대조했다.
- 증거 archive SHA-256: `613677346708b23c140bc8d6307d690b7f746b1e57146bcdb3f21f13c2023da6`.

## 7. 현재 단계와 후속 작업

실기 탐색 단계가 완료됐다. 다음 구현 설계에서는 객체별 backend 식별, 소유권·메타데이터 처리, 스냅샷 범위와 실패 복구 절차를 확정한다. 기존 ZFS 기본값을 유지하는 선택형 Btrfs 방향을 계속 추천한다.

이번 범위에서 미검증인 항목은 PureCVisor API/CLI/UI 통합, guest 외부 통신, 일반 사용자·user namespace 실행, quota, send/receive 백업, ENOSPC·전환 중 중단 복구, 호스트 재부팅과 기존 Ubuntu ZFS 회귀다. 이 조건을 통과하기 전에는 PureCVisor의 Btrfs 지원을 출시 완료로 표시하지 않는다.
