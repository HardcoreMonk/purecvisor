# UEFI VM 삭제 NVRAM 수정 운영 인계

> 이 문서는 `5e84387` 수정 회차의 증거다. 이후 동일 Arch 시험 호스트에는 이 수정을 포함한
> Btrfs 구현 `e028ef2`를 배포했다. 현재 배포 바이너리·설정·시험 자원 정리 결과는
> [Btrfs API 인계](2026-09-16-lxc-btrfs-api-validation.md)를 따른다.

## Release Scope

Ubuntu와 Arch의 공통 `vm.delete` worker를 수정했다. NVRAM을 보존한 채 libvirt 정의를
해제하고 디스크 정리가 성공한 뒤 NVRAM을 삭제한다. 저장소 실패 때 UEFI 설정을 유지하며,
디스크 접근 권한 오류를 파일 부재로 오인하지 않는다. XML/미지원 NVRAM 저장 형식은
삭제 전에 거부한다. 마지막 NVRAM 정리 실패는 실제 fail audit와 수동 정리 경로를 남긴다.
제품 버전은 `2.0.0`을 유지하며 신규 버전 태그나 패키지 릴리스를 발급하지 않았다.

- 계약: [ADR-0017](../adr/0017-vm-delete-atomicity-rollback.md)
- 회귀: [실제 worker 실행 테스트](../../scripts/tests/test_vm_delete_nvram.py)
- 기능 기준: [서비스 시나리오 5.15](../SERVICE_FUNCTIONAL_TEST_SCENARIOS.md#515-uefi-vm-삭제와-nvram-보존)
- 공개 C 소스 SHA-256: `48545180d5843578e26898fc4a51515e7526cf45df944b0719a60a05d1730932`

## Verification

| 검증 | 결과와 범위 |
|---|---|
| 회귀 RED → GREEN | 기존 NVRAM 누락 및 디스크 접근 실패의 거짓 성공을 각각 재현한 뒤 최종 14개 시나리오 PASS |
| 반사실 | NVRAM 정리 제거, 저장소보다 먼저 NVRAM 삭제 두 변형 모두 파일 효과 회귀가 거부 |
| ASan/UBSan | 최종 14개 시나리오 PASS |
| Ubuntu 빌드 | 공개판 debug/release, 개발판 debug 증분 빌드 PASS·변경 컴파일 경고 0 |
| 기존 C suite | `make -j1 test`: 1,479 PASS·14 환경 SKIP, audit startup 별도 5 PASS |
| 공개 정적 게이트 | `make -j1 check-all`: 40개 PASS, 공개 C/JS 설명 주석 0 |
| 최종 보완 게이트 | 실제 worker 회귀, audit/OVA/DPDK 배선, 주석 정책·spec 정합·diff PASS |
| AppArmor | 기본 미부착 정책 게이트 및 프로필 실제 컴파일 PASS; 커널에 프로필을 로드하지 않음 |
| Ubuntu 실기 | 기존 코드 오류 재현 → 수정 성공, BIOS 실행 중 삭제, UEFI 실행 중 삭제, 디스크 실패·설정 보존·재시도, 마지막 NVRAM 실패 보고의 5개 시나리오 PASS |
| Arch 실기 | 정상 API의 UEFI 최초 부팅 전 삭제, UEFI 실행 중 삭제, BIOS 실행 중 삭제, 디스크 실패·설정 보존·재시도, 마지막 NVRAM 실패 audit의 5개 시나리오 PASS |

Ubuntu 실기는 실제 worker·libvirt·파일시스템을 사용하고 DPDK/ZFS/보안 그룹 연동은 대역이다.
기존 VM 3대의 UUID·상태·비활성 XML 해시를 보존했다. Ubuntu 운영 daemon은 교체하지 않았다.
Arch는 설치 서비스에 native release를 반영하고 HTTPS/API 접수, `vm.delete` 실제 audit,
virsh, 디스크와 NVRAM 실물을 함께 대조했다. 최종 VM 목록과 테스트 자원 잔여는 0이다.

기존 전체 suite·40개 게이트는 이번 변경의 첫 검증 회차이며, 마지막 디스크 접근 오류
보강 후에는 영향받는 14개 회귀·ASan/UBSan·빌드·관련 게이트와 양쪽 실기 5개씩을 재실행했다.
이는 전체 제품 감사, 모든 저장소/펌웨어 조합, 장시간 안정성 인증을 의미하지 않는다.

## Audit

Arch 정상 삭제는 `result=ok`, 의도적으로 주입한 파일/NVRAM 삭제 실패는 `result=fail`을
확인했다. rollback 뒤 재시도는 `ok`로 끝났다. callback의 기존 fire-and-forget audit를
유지하며 접수 응답이나 목록에서 사라지는 것만으로 성공을 판정하지 않았다.

같은 에이전트가 별도 diff 리뷰로 XML 경계, 경로 소유권, 조기 반환 cleanup, 기본 undefine
폴백, AppArmor, 실패 감사와 저장소 rollback을 검토했다. 독립 에이전트 리뷰는 수행하지 않았다.
리뷰에서 발견한 디스크 접근 오류의 파일 부재 오판을 추가 RED/GREEN으로 시정했다.

## Blockers

이 수정의 소스·지정 실기 검증 blocker는 없다.

## Warnings

- 최초 `make check-all` 병렬 실행은 FE 계약 검사의 임시 주석 probe를 공개 주석 검사가
  동시에 읽어 실패했다. 임시 파일 정리를 확인하고 순차 실행해 통과했다.
- 첫 sanitizer 실행의 libvirt 초기화가 없는 대역 harness를 초기화하도록 보정했다.
- Arch 실기 harness의 잘못된 `network_mode=nat`는 API에서 거부됐다. 실제 제품 계약인
  `network_mode=bridge`, 기존 NAT bridge를 사용해 최종 시험을 통과했다.
- 기존 전체 suite의 환경 SKIP 14개를 PASS로 계상하지 않았다.

## Residual Risk

파일형 NVRAM만 지원한다. block/network NVRAM과 새 `varstore`는 사전 거부한다.
실제 ZFS pool 실패, Secure Boot 키/서명, 삭제 중 프로세스/호스트 중단 복구는 미검증이다.
NVRAM unlink만 실패하면 디스크 삭제 뒤 설정 파일이 남을 수 있으므로 오류 경로를 수동
정리해야 한다. 기본 경로 밖 NVRAM의 AppArmor 접근 권한은 운영 프로필에서 관리한다.

## Current Lifecycle Stage

operate. Arch 적용 바이너리 SHA-256:
`0e676357e7a3fdf7b53560726986b397485e62f13b1a2a663a1f00cd64a720be`.
Ubuntu는 소스·실기 검증까지 완료했고 기존 운영 daemon 교체는 하지 않았다.

## Next Action · Follow-Up Tasks

Ubuntu 운영 설치본에 반영하려면 해당 호스트에서 수정 커밋을 빌드·배포해야 한다.
이번 작업에서는 다른 Ubuntu 운영 호스트의 서비스 교체를 수행하지 않았다.

원시 증거는 공개 저장소 로컬 `.git/nvram-fix-20260916/`에 보관한다. SSH/관리자 암호,
JWT와 실제 호스트 설정은 공개 커밋에 포함하지 않는다.
