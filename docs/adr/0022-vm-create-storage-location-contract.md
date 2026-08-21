# ADR-0022: VM 생성 저장 위치 계약

날짜: 2026-04-28
상태: accepted
Single Edge 적용 상태: 활성

## 맥락

VM 생성 UI는 VMware Workstation 계열 생성 흐름처럼 디스크 타입과 저장 위치를 사용자가 명시할 수 있어야 한다.

기존 구현은 `storage_type`만 전달했고, 실제 저장 위치는 데몬 설정의 기본값에 고정되어 있었다.

- zvol: `pcv_config_get_zvol_pool()` 값 아래에만 생성
- qcow2/raw: `pcv_config_get_image_dir()` 값 아래에만 생성

이 구조에서는 UI에서 저장 위치를 선택하더라도 백엔드가 반영할 수 없고, 자동 감지 모드도 기본 위치만 기준으로 판단한다.

## 결정

`vm.create`의 저장 위치 계약을 다음처럼 고정한다.

| 파라미터 | 의미 |
|----------|------|
| `storage_type` | `zvol`, `qcow2`, `raw` 중 하나. 생략하면 자동 감지 |
| `storage_pool` | zvol 부모 ZFS 데이터셋. 예: `tank/vms` |
| `image_dir` | qcow2/raw 파일 디스크 저장 디렉터리. 예: `/var/lib/libvirt/images` |
| `storage_location` | 하위 호환용 단일 위치 값. 타입에 따라 `storage_pool` 또는 `image_dir`로 매핑 |

자동 감지 규칙:

1. `storage_type`이 `zvol`이면 `storage_pool` 또는 기본 `zvol_pool`이 반드시 존재해야 한다.
2. `storage_type`이 `qcow2` 또는 `raw`이면 `image_dir` 또는 기본 `image_dir` 아래에 파일 디스크를 만든다.
3. `storage_type`이 없으면 `storage_pool` 또는 기본 `zvol_pool` 존재 여부를 먼저 본다.
4. ZFS 부모 데이터셋이 있으면 zvol을 만들고, 없으면 `image_dir`에 qcow2로 폴백한다.

입력 검증 규칙:

1. `storage_pool`은 절대 경로가 아니어야 하며, `..`, `//`, `@`를 허용하지 않는다.
2. `storage_pool`의 각 문자는 영문/숫자/`_`/`-`/`.`/`/`만 허용한다.
3. `image_dir`는 절대 경로여야 한다.
4. `image_dir`는 `/`, `/etc`, `/usr`, `/proc`, `/sys`, `/dev`, `/run`, `/root` 같은 시스템 루트 또는 그 하위 경로를 허용하지 않는다.

## 구현 기준

프론트엔드:

- VM 생성 3단계에서 디스크 타입, 저장 위치, 예상 디스크 경로를 함께 보여준다.
- zvol/auto 모드에서는 `storage_pool` 입력과 ZFS 풀 선택 UI를 제공한다.
- qcow2/raw 모드에서는 `image_dir` 입력을 제공한다.
- 단계 이동은 기존 모달 스택에 push하지 않고 같은 모달 내용을 교체한다.
- 완료 시 `closeModal(true)`로 생성 마법사 스택을 강제로 정리한다.

백엔드:

- `dispatcher.c`는 `storage_pool`, `image_dir`, `storage_location`을 파싱하고 검증한다.
- 선택된 저장 위치에서 기존 VM 디스크 충돌을 먼저 확인한 뒤 accepted 응답을 보낸다.
- `vm_manager.c`는 전달받은 저장 위치를 실제 zvol/qcow2/raw 생성 위치로 사용한다.
- `vm.delete`는 기본 ZFS 풀을 가정하지 않고 libvirt XML의 실제 `<source dev='/dev/zvol/...'>` 값을 기준으로 zvol 삭제 대상을 계산한다.

## 결과

좋음:

- UI에서 선택한 저장 위치가 실제 VM 디스크 생성 위치와 일치한다.
- 자동 감지 모드도 운영자가 지정한 ZFS 부모 데이터셋과 파일 디렉터리를 기준으로 동작한다.
- 커스텀 ZFS 부모 데이터셋에 생성한 VM도 삭제 시 같은 위치의 zvol을 정리한다.

주의:

- 스냅샷/백업 등 일부 기존 보조 기능은 아직 기본 `zvol_pool` 중심 구현이 남아 있을 수 있다.
- 커스텀 `storage_pool`을 사용하는 VM의 보조 기능을 확장할 때는 libvirt XML의 실제 디스크 경로를 우선 읽어야 한다.

## 하지 않기로 한 것

- `storage_pool`에 절대 경로를 허용하지 않는다. ZFS dataset 이름은 파일 시스템 경로가 아니다.
- `image_dir`에 시스템 루트 하위 경로를 허용하지 않는다. 잘못된 입력으로 시스템 파일을 덮거나 운영체제 영역에 VM 디스크를 만들 수 있기 때문이다.
- UI에서 아직 지원하지 않는 VMware Workstation의 모든 세부 디스크 옵션을 한 번에 추가하지 않는다. 이번 결정은 저장 위치 계약을 먼저 고정한다.
