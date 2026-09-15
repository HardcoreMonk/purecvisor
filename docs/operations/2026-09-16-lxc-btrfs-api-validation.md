# 선택형 LXC Btrfs API 검증 인계

> 검증일: 2026-09-16 KST
> 범위: [ADR-0058](../adr/0058-lxc-storage-backend-identity.md)의 선택형 컨테이너 Btrfs 구현
> 판정: 지정 Arch/Btrfs 호스트의 API·복구·관측성·정리 검증 통과

## 1. 구현 범위

기본값은 ZFS이며 Btrfs는 설정으로 명시 선택한다. 저장소 모듈이 실제 filesystem/subvolume identity, 읽기 전용 snapshot, rootfs 원자 교환과 복원 journal, 검증된 관리 데이터 삭제를 담당한다. driver는 객체 잠금과 정지 조건을 적용하고 작업 완료 callback은 Job·WebSocket·audit 결과를 기록한다.

기존 컨테이너는 생성 당시 저장소 신원으로 처리한다. 설정 변경만으로 backend를 이동하지 않는다. Btrfs는 privileged LXC의 정지 상태 복제·snapshot·복원만 지원한다. nested subvolume·외부 mount·rootless·quota·send/receive 제품 백업·자동 migration은 지원 범위 밖이다.

## 2. 로컬 검증

- 저장소 모듈 실제 C 실행 회귀 24개와 UUID 대조를 제거한 변이 검사.
- driver·UI·health·CPU 설정 실행 회귀 7개 그룹, snapshot 요청자·완료 audit 12개 조합.
- ASan/UBSan, C23 경고 검사, `make single`, `make test`, `make check-all`, `make release`.
- owner/RBAC, 비동기 audit/WS, 공개 자체 소스 설명 주석 0, UI 구문과 가이드 표면 검사.
- 명세와 코드 품질 검토에서 임시 파일 재시도, 외부 ZFS mount 식별, 설정 변경·health 자동 재시작 잠금, snapshot 호출자 보존을 확인했다.

## 3. 실제 시험 환경

| 항목 | 환경 |
|---|---|
| 호스트 | Omarchy 4.0.3, Arch 계열 x86_64 |
| 커널 | `7.2.3-arch1-3` |
| LXC / Btrfs 도구 | `lxc 1:7.0.0-2` / `btrfs-progs 7.1-1` |
| 파일시스템 | Btrfs, root가 관리하는 전용 시험 디렉터리 |
| ZFS | 미설치 |
| 후보 빌드 | 호스트에서 최적화·LTO 릴리스 전체 재빌드, 경고 0 |
| 배포 | 기존 바이너리·설정·UI 백업 후 후보 교체, HTTPS 인증서 검증과 health 확인 |

## 4. 실기에서 발견한 문제와 시정

첫 API 생성은 native LXC가 새 관리 디렉터리를 root:root `0770`으로 만들면서 엄격한 권한 검사에 거부됐다. 실패 Job과 새 rootfs는 보존됐다. 시험에서 생성한 경로·실제 UUID·정지 상태를 확인한 뒤 해당 객체만 native LXC로 정리했다.

새 객체 기록 단계에서만 신뢰할 수 있는 부모 fd로 경로를 열고 소유권·기존 marker·복구 상태를 확인한 뒤 그룹 쓰기 권한을 제거한다. 기존 데이터나 다른 소유자·그룹·symlink·world-write 경로의 권한을 자동 변경하지 않는다. 관련 정상·거부 10개 조합을 회귀에 추가했다. 첫 실패 증거도 최종 성공 기록과 함께 보존한다.

LXC 7은 기존 `lxc.cgroup.cpu.shares` 키를 거부했다. 생성 시 cgroup v2 `cpu.weight`도 기록하고 두 버전의 설정이 모두 실패하면 성공으로 보고하지 않도록 보완했다. 1→100, 2→200과 상한·두 setter 실패를 검증한다. 가중치는 상대 CPU 배분이며 코어 수 상한과 다르다. [Linux kernel의 weight 계약](https://cdn.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html#weights)을 따른다.

초기 정리 검사는 일반 디렉터리에 실행한 `btrfs subvolume list -o`가 상위 `@`의 기존 자식 서브볼륨도 반환하는 특성을 잘못 해석했다. API 삭제와 시험 경로 정리는 성공했고 기존 호스트 경로는 보존됐다. mount의 FSROOT·TARGET으로 시험 경로의 filesystem 상대 경로를 계산해 잔여 서브볼륨을 판정하도록 하네스를 시정했다.

## 5. API 결과와 호스트 정리

| 검증 묶음 | 확인한 최종 결과 |
|---|---|
| API 생성·lifecycle | 실제 rootfs UUID/ID, 요청자 owner, 이미지 메타데이터, guest systemd 부팅, IPv4, memory.max=536870912, 요청 2의 cpu.weight=200, 파일 재시작 영속화 |
| snapshot·복제 | 읽기 전용 snapshot, 파일 복원, config/owner/meta SHA-256 보존, CoW clone의 Parent UUID와 부팅·쓰기 분리, snapshot 목록·삭제 |
| 거부·보존 | 실행 중 snapshot, 중복 생성, rootless, nested subvolume, 변조된 filesystem UUID·rootfs 경로, fstab·tmpfs bind mount 거부. 복구 후 재부팅 가능 |
| 중단 상태 복구 | 실제 UUID/ID와 원자 교환으로 만든 교환 전·후 journal 상태에서 API 시작이 복구 완료. 원래 파일과 현재 sidecar 보존 |
| 삭제 재시도 | 부분 snapshot 삭제와 marker/journal/snapshot/tombstone 대상 임시 파일 4개를 남긴 상태에서 API 삭제 완료 |
| 결과 채널 | snapshot 생성 성공·실행 중 거부·삭제 각각의 terminal Job과 정확히 1개 WS job.complete, 정확히 1개 완료 audit의 요청자·대상·결과 일치 |
| 기본값 변경 | ZFS 기본값으로 변경·데몬 재시작 후에도 원본·clone의 Btrfs 부팅·파일, snapshot 생성·복원·삭제 정상 |
| 정리 | API/native 컨테이너와 VM 목록, 시험 관리 경로와 해당 서브볼륨 잔여 없음. 서비스 active |

중단 검사는 실제 Btrfs 위에 정확한 journal·교환 상태를 구성한 시험이다. 데몬 강제 종료나 호스트 정전 시험으로 표현하지 않는다. 실패했던 초기 실행과 하네스 시정 기록을 지우지 않고 최종 실행과 구분했다.

운영 설정은 `storage_backend=btrfs`, `rootless=false`, `lxc_path=/var/lib/purecvisor/lxc`로 마무리했다. 테스트 전용 경로와 혼용하지 않는다. 원래 바이너리·설정·UI와 후보별 변경 전 상태는 시험 호스트의 비공개 백업에 보존한다.

## 6. 운영 경계와 후속 검증

이 회차는 지정 Arch/Btrfs 호스트의 단기 기능 검증이다. 모든 Arch 설치, 장시간 안정성, 호스트 재부팅·정전·ENOSPC, 실제 Ubuntu ZFS 전체 회귀와 전체 소스 감사 완료를 뜻하지 않는다. 기존 native LXC 탐색 결과는 [이전 실기 인계](2026-09-16-lxc-btrfs-live-validation.md)에서 구분한다.

초기 `2.0.0` 태그는 ZFS 전용 생성 경로다. 이 변경을 포함한 공개 소스를 빌드해야 선택형 Btrfs를 사용할 수 있다. 제품 버전·태그 인상은 이번 작업에 포함하지 않는다.

## 7. 배포 증거

- 설치 바이너리 SHA-256: `dcbe226924183478899147bdca1db76a78cf680dd9ae53a65c3f4cd35600882c`.
- 빌드 입력 311개 파일의 SHA-256 목록을 후보 소스와 대조해 일치를 확인했다.
- 호스트 사후 비교 13개 항목 통과: kernel·패키지·VM/LXC·route·주소·기존 subvolume, 의도한 설정 변경만 존재, 최종 API 목록·시험/운영 경로 비어 있음, 서비스 정상·설치 바이너리 일치.
- subvolume generation과 DHCP 유효 시간은 일상적으로 변하므로 식별자·경로 및 네트워크 구성과 구분했다.
- 비공개 원시 증거에는 최초 실패, 최종 성공, API·명령·복구 상태, WS/audit 대조, before/after와 백업 위치를 함께 보존한다.
- 원시 증거 archive SHA-256: `85845dfadad3dd5a781250d73f6e4b534d2dbb9491285b0378cb1b505d0b697b` (326개 파일).
- 최종 내장 가이드·JS·서비스 워커는 실제 HTTPS 응답의 SHA-256과 배포 파일을 대조했다.
