# ADR-0023: VM clone 안전장치

날짜: 2026-04-28
상태: accepted
Single Edge 적용 상태: 활성

## 맥락

오픈 베타 운영 중 실제 VM을 full clone하면서 게스트 OS 내부 식별자가 중복되는 문제가 확인되었다.

VM 디스크를 블록 단위로 복제하면 호스트 XML의 VM 이름, UUID, MAC만 바꾸어도 충분하지 않다. 게스트 OS 내부에는 다음 값이 그대로 남는다.

- `/etc/machine-id`
- DHCP DUID와 lease cache
- hostname
- SSH host key
- cloud-init state
- LVM PV UUID, VG UUID, VG name 같은 LVM metadata
- filesystem UUID

Ubuntu뿐 아니라 Rocky/RHEL/Alma/CentOS/Fedora 등 LVM을 쓰는 Linux 계열에서도 같은 문제가 발생할 수 있다. qcow2/raw 파일 디스크의 경우에는 별도 파일 복제 없이 XML만 바꾸거나, raw 경로를 그대로 공유하면 데이터 손상으로 이어질 수 있다.

## 결정

`vm.clone`은 host-level disk/XML clone 뒤 guest identity reset을 명시적으로 처리하는 경로로만 허용한다.

현재 허용 조건:

1. 호출자는 admin이거나, operator로서 source VM owner-scope를 통과해야 한다.
2. 요청에 `template_prepared=true` 또는 `clone_safety_ack="template-prepared"`가 있으면 운영자가 guest identity를 이미 정리한 템플릿으로 본다.
3. 템플릿 확인이 없으면 `guest_reset=true` 기본값으로 target disk에 libguestfs 기반 guest reset을 수행해야 한다.
4. 소스 VM은 data disk가 정확히 1개여야 한다.
5. 해당 disk는 ZFS zvol, qcow2 file, raw/img file 중 하나여야 한다.
6. clone 로직은 기본 `zvol_pool`이나 `image_dir`을 추측하지 않고 libvirt XML의 실제 `<source dev='...'>` 또는 `<source file='...'>` 경로를 기준으로 source/target 경로를 계산한다.
7. source VM은 `shut off` 상태여야 한다. power on 상태의 clone은 storage type과 무관하게 preflight에서 거부한다.
8. qcow2/raw는 `mode=full`만 허용한다.

현재 차단 조건:

1. data disk가 0개 또는 2개 이상인 VM clone
2. unsupported disk source clone
3. qcow2/raw `mode=cow`
4. power on 상태의 source VM clone
5. `template_prepared=true`도 없고 `guest_reset=true`도 아닌 clone
6. `guest_reset=true`인데 `libguestfs-tools` 또는 그 도구인 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`를 사용할 수 없는 host

## 구현 기준

백엔드:

- `vm.clone` 핸들러는 `source`/`name`/`vm_id`, `clone_name`/`target`/`new_name` 호환 파라미터를 받는다.
- accepted 응답 전에 libvirt XML을 읽어 data disk 개수와 디스크 타입을 확인한다.
- 디스크 판별과 target 경로 계산은 `src/modules/virt/vm_clone_plan.c`로 분리한다. dispatcher는 요청/권한/응답을 맡고, clone plan 모듈은 XML의 data disk가 zvol/qcow2/raw 중 무엇인지 판별한다.
- zvol clone은 XML에서 읽은 실제 dataset을 기준으로 수행한다.
- zvol full clone은 `zfs send` stdout을 `zfs recv` stdin으로 직접 연결하는 `pcv_spawn_pipe_sync()` 스트리밍 경로를 사용한다. `/bin/sh -c`, 셸 파이프, 리다이렉션, `/tmp` 대용량 임시 파일은 사용하지 않는다.
- zvol full clone 성공 후에는 독립 target dataset이 만들어졌으므로 source 임시 snapshot을 best-effort로 삭제한다. CoW clone은 origin 의존성이 있으므로 snapshot을 유지한다.
- snapshot 생성 이후 ZFS 복제, XML 재조회, disk source 치환, domain define이 실패하면 target dataset을 먼저 best-effort 삭제하고, source 임시 snapshot이 더 이상 origin으로 필요하지 않은 실패 경로에서는 source 임시 snapshot도 best-effort 삭제한다.
- source VM이 power on 상태이면 zvol/qcow2/raw 모두 preflight에서 거부한다.
- qcow2/raw는 clone plan 단계에서 같은 디렉터리 아래의 새 target file path를 계산한다. qcow2는 `.qcow2`, raw는 기존 `.raw`/`.img` 확장자 유지 또는 `.img` 기본값을 사용한다.
- qcow2/raw full copy는 worker에서 `qemu-img convert -f <format> -O <format> <source> <target>` argv 배열로 실행한다. 원본 파일 공유, 상대 경로, 같은 source/target, 이미 존재하는 target은 거부한다.
- 템플릿 확인이 없는 일반 VM clone은 target disk 생성 후, domain define 전에 libguestfs 기반 guest reset을 실행한다.
- guest reset은 `virt-sysprep`으로 `defaults`, `fs-uuids`, `lvm-uuids`, `lvm-system-devices`, `net-hostname`, `net-hwaddr`, hostname 설정, cloud-init cleanup을 수행한다.
- `virt-sysprep fs-uuids`가 ext filesystem UUID를 실제로 바꾸지 못하는 환경을 막기 위해, `virt-filesystems`로 ext2/3/4 filesystem을 수집하고 `guestfish`의 `e2fsck-f` + `set-uuid-random`으로 target ext filesystem UUID 변경을 강제한다.
- filesystem UUID 변경 뒤에는 `virt-customize`로 `/etc/fstab`의 UUID 참조를 새 값으로 갱신하고, Ubuntu `update-initramfs`, Rocky/RHEL 계열 `dracut`, grub 재생성, SELinux `/.autorelabel` 계약을 실행한다.
- `libguestfs-tools`가 제공하는 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish` 중 하나라도 없으면 일반 VM clone은 preflight에서 거부된다. 준비된 템플릿 clone은 `template_prepared=true`로 guest reset을 생략할 수 있다.
- `virDomainDefineXML` 실패 또는 XML 재조회 실패 시 생성된 target dataset을 best-effort로 롤백한다.
- qcow2/raw 실패 경로는 target file을 best-effort로 삭제한다.
- XML 패치 시 VM 이름, UUID, MAC, disk source를 분리 처리한다. MAC 주소 재생성은 원본 XML을 한 번만 스캔하는 방식이어야 한다. 새 MAC도 같은 정규식에 다시 매칭되므로 반복 검색/치환 방식은 worker 무한 루프를 만들 수 있다.

CLI:

- 준비된 템플릿 복제는 `pcvctl vm clone <source> <clone_name> --template-prepared`로 명시한다.
- 일반 VM guest reset clone은 `pcvctl vm clone <source> <clone_name> --guest-reset` 또는 기본 guest reset 경로를 사용한다.

Web UI:

- VM clone modal은 source VM이 power on 상태이면 clone 버튼을 비활성화하고 종료 후 재시도 안내를 표시한다.
- VM clone modal은 target 이름, `cow`/`full`, guest reset 또는 prepared template 선택을 API에 전달한다.

## 후속 개발 조건

일반 VM clone과 qcow2/raw clone 실행 경로를 연 뒤, Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol 실환경 회귀 검증까지 완료했다. Rocky/RHEL 계열과 SELinux enforcing boot smoke는 코드 계약을 유지하되, 제품 범위 확대 전 문서상 후속 검증 항목으로만 남긴다.

1. 완료: zvol/qcow2/raw별 source disk resolver와 target disk path creator 분리
2. 완료: zvol full clone의 send/recv 스트리밍 경로와 full clone 임시 snapshot cleanup
3. 완료: qcow2 full-copy primitive를 `qemu-img convert -f qcow2 -O qcow2`로 구현하고 worker에 연결했다.
4. 완료: raw full-copy primitive를 `qemu-img convert -f raw -O raw`로 구현하고 worker에 연결했다. 원본 파일 공유 금지와 target 선점 방지는 단위 테스트로 고정했다.
5. 완료: `virt-sysprep` identity reset, `guestfish` ext filesystem UUID 보정, `virt-customize` fstab/boot artifact rebuild 파이프라인
6. 완료: `lvm-uuids`와 `lvm-system-devices` 기반 LVM PV/VG UUID 분리
7. 완료: Ubuntu 계열 `update-initramfs`와 grub 재생성 command 계약
8. 완료: Rocky/RHEL 계열 `dracut`, `grub2-mkconfig`, SELinux relabel command 계약
9. 완료: Ubuntu 24.04 non-LVM qcow2/raw full clone + guest reset + boot smoke 실환경 회귀 테스트
10. 완료: Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone + guest reset + boot smoke 실환경 회귀 테스트
11. 문서상 후속: Rocky/RHEL LVM, SELinux enforcing guest boot smoke

## 실환경 검증 기록

2026-04-28 KST에 `example-vm-source` VM을 원본으로 Single Edge 실서버에서 clone 기능을 검증했다.

검증 조건:

- 원본 VM: `example-vm-source`
- 원본 disk: `/dev/zvol/rpool/example-vm-source`
- daemon 기본 `zvol_pool`: `pcvtntank/vms`
- clone 대상: `example-vm-clone`
- mode: `cow`
- 명시 확인: `--template-prepared`

확인한 회귀:

1. 구버전 clone 로직은 기본 `zvol_pool`을 기준으로 source dataset을 추측해 실패했다. 이 회귀는 실제 XML의 `/dev/zvol/rpool/example-vm-source` 경로를 기준으로 plan을 계산하도록 수정해 차단했다.
2. MAC 주소 재생성 함수가 새 MAC도 다시 매칭해 worker thread가 CPU를 계속 사용하는 무한 루프에 빠질 수 있었다. `g_regex_replace_eval()`로 원본 XML을 한 번만 스캔하도록 수정했다.
3. 후속 개발에서 `pcv_vm_clone_build_disk_plan()`을 추가해 zvol/qcow2/raw target 경로 계산을 공통화했다.
4. 후속 개발에서 `pcv_spawn_pipe_sync()`를 추가해 zvol full clone의 `zfs send` -> `zfs recv` 경로를 셸과 `/tmp` 임시 파일 없이 스트리밍 처리하도록 변경했다. full clone 성공 후 source 임시 snapshot은 정리하고, CoW clone snapshot은 origin 의존성 때문에 유지한다.
5. 후속 개발에서 snapshot 생성 이후 실패 cleanup helper를 추가했다. ZFS 복제나 libvirt XML/domain define 단계가 실패하면 target dataset을 먼저 정리한 뒤 source 임시 snapshot이 불필요한 경우 함께 정리한다. accepted 응답에는 worker completion과 같은 `job_id`를 포함한다.
6. 후속 개발에서 qcow2/raw full clone worker 연결과 일반 VM guest reset 경로를 추가했다. 템플릿 확인이 없는 clone은 `virt-sysprep`, `virt-filesystems`, `guestfish`, `virt-customize`로 target disk의 machine-id, DHCP/SSH/cloud-init 상태, LVM PV/VG UUID, ext filesystem UUID, hostname, fstab UUID 참조, initramfs/grub/SELinux relabel 계약을 처리한다. source VM은 storage type과 무관하게 `shut off`여야 하며, qcow2/raw는 추가로 `mode=full`에서만 허용한다.

최종 성공 증거:

- `pcvctl vm clone example-vm-source example-vm-clone --mode cow --template-prepared`가 `accepted` 반환
- 응답에 `source_disk=/dev/zvol/rpool/example-vm-source`, `target_disk=/dev/zvol/rpool/example-vm-clone` 포함
- `virsh list --all`에서 `example-vm-clone`가 `shut off` persistent domain으로 표시
- `zfs list rpool/example-vm-clone`의 origin이 `rpool/example-vm-source@clone-example-vm-clone`
- clone VM UUID와 MAC이 원본과 다름
- `/var/log/purecvisor/audit.log`에 `vm.clone` 결과 `ok` 기록

2026-04-29 KST에는 zvol full clone 스트리밍 경로를 재배포한 뒤 같은 원본 VM으로 추가 실검증했다.

full clone 성공 증거:

- `pcvctl --format=json vm clone example-vm-source example-vm-full-clone --mode full --template-prepared`가 `accepted` 반환
- 응답에 `source_disk=/dev/zvol/rpool/example-vm-source`, `target_disk=/dev/zvol/rpool/example-vm-full-clone` 포함
- `virsh dominfo example-vm-full-clone`에서 `shut off`, `Persistent: yes` 확인
- `zfs list -o origin rpool/example-vm-full-clone`의 origin이 `-`로 표시되어 CoW가 아닌 독립 zvol임을 확인
- `rpool/example-vm-source@clone-example-vm-full-clone` 임시 snapshot이 full clone 성공 후 남지 않음
- clone VM UUID와 MAC이 원본과 다름
- `/var/log/purecvisor/audit.log`에 `target=example-vm-source:example-vm-full-clone`, `result=ok` 기록
- 검증용 domain, zvol, snapshot은 검증 종료 후 모두 제거

같은 날짜의 후속 개발에서 qcow2/raw 파일 디스크용 full-copy primitive를 추가했다.

파일 디스크 primitive 검증 증거:

- `pcv_vm_clone_copy_file_disk()`가 qcow2/raw plan만 허용하고 zvol plan은 거부
- qcow2 source image를 `qemu-img convert -f qcow2 -O qcow2`로 별도 target 파일에 복제
- raw source image를 `qemu-img convert -f raw -O raw`로 별도 target 파일에 복제
- target 파일이 이미 존재하면 복제를 시작하지 않고 기존 파일을 보존
- `pcv_vm_clone_disk_plan_beta_allowed()`는 zvol/qcow2/raw 단일 data disk plan을 허용
- guest reset argv와 cleanup guard가 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`, `fs-uuids`, `lvm-uuids`, `lvm-system-devices`, `set-uuid-random`, `update-initramfs`, `dracut`, `grub2-mkconfig`, `/.autorelabel` 계약을 포함

2026-04-29 KST에는 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol source VM으로 실제 `vm.clone` + guest reset + boot smoke를 검증했다.

검증 조건:

- non-LVM base image: `noble-server-cloudimg-amd64.img`
- non-LVM checksum: Ubuntu cloud image `SHA256SUMS` 기준 OK
- non-LVM source VM: `pcv-clone-ubuntu-qcow2-src`, `pcv-clone-ubuntu-raw-src`
- non-LVM clone VM: `pcv-clone-ubuntu-qcow2-copy2`, `pcv-clone-ubuntu-raw-copy2`
- LVM source VM: `pcv-u2404-lvm-qcow2-src`, `pcv-u2404-lvm-raw-src`, `pcv-u2404-lvm-zvol-src`
- LVM clone VM: `pcv-u2404-lvm-qcow2-copy2`, `pcv-u2404-lvm-raw-copy2`, `pcv-u2404-lvm-zvol-copy2`
- source state: 모두 `shut off`
- mode: `full`
- guest reset: `true`

Ubuntu non-LVM 확인:

- `virt-filesystems --all --long --uuid` 결과가 `/dev/sda1`, `/dev/sda14`, `/dev/sda15`, `/dev/sda16`, `/dev/sda`만 표시했다.
- LVM LV/PV/VG 항목은 표시되지 않았다.
- qcow2/raw clone의 ext4 root/boot filesystem UUID가 source와 달라졌고, `/etc/hostname`은 clone VM 이름으로 변경됐으며 `/etc/machine-id`는 재생성됐다.

Ubuntu LVM 확인:

- LVM source는 `/boot` ext4와 LVM root LV 조합이다.
- qcow2/raw/ZFS zvol clone 모두 `/boot`와 root ext4 filesystem UUID가 source와 달라졌다.
- qcow2/raw/ZFS zvol clone 모두 LVM PV UUID와 VG UUID가 source와 달라졌다.
- `/etc/fstab`의 root/boot UUID 참조가 clone target의 새 UUID로 갱신됐다.
- `/etc/hostname`은 각각 clone VM 이름으로 변경됐고 `/etc/machine-id`는 재생성됐다.

Ubuntu clone 성공 증거:

- `vm.clone` accepted 응답에 `storage_type=qcow2` 또는 `storage_type=raw`, `guest_reset=true`, `job_id`, 실제 `source_disk`/`target_disk`가 포함됐다.
- ZFS zvol clone 응답에도 `storage_type=zvol`, `guest_reset=true`, `job_id`, 실제 zvol `source_disk`/`target_disk`가 포함됐다.
- worker가 `qemu-img convert` 또는 ZFS full clone 후 `virt-sysprep`, `virt-filesystems`, `guestfish`, `virt-customize`를 실행하고 clone domain을 정의했다.
- `/var/log/purecvisor/audit.log`에 non-LVM qcow2/raw와 LVM qcow2/raw/ZFS zvol clone 5건이 모두 `result=ok`로 기록됐다.
- clone domain은 define 직후 모두 `shut off` 상태로 남았다.
- `qemu-img info` 기준 qcow2 target은 qcow2, raw target은 raw이며 backing file 없이 별도 target file로 생성됐다.
- ZFS zvol full clone target은 `origin=-`인 독립 zvol로 확인했다.
- 모든 Ubuntu clone VM을 수동으로 시작한 뒤 `virsh domstate --reason`이 `running (booted)`를 반환했다.

남은 영역의 결정:

- Rocky/RHEL LVM, SELinux enforcing boot smoke는 문서상 후속 검증 항목으로만 유지한다.
- 해당 계열의 boot artifact 재생성 command 계약은 코드와 단위 테스트에서 유지하지만, 현재 Ubuntu 완료 판정의 필수 실환경 gate로 보지 않는다.

## 후속 진행 상태

2026-04-29 KST 재확인에서는 작업트리 기준으로 다음 검증을 다시 통과했다.

- `make single`
- `./test_runner -p /spawn_launcher`
- `./test_runner -p /vm_clone_plan`
- `make test`
- `scripts/check_audit_placement.py`
- `make check-rbac`
- `node --check ui/app.bundle.js`
- clone 관련 `git diff --check`

이 재확인은 실환경 CoW/full clone 성공 기록이 문서와 코드 검증 기준에 반영됐는지 확인하기 위한 절차다. 제품 범위는 준비된 단일 zvol 템플릿 clone에서 libguestfs 기반 일반 VM clone, Ubuntu 24.04 non-LVM qcow2/raw full clone, Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone 실행 경로까지 확장됐다. Rocky/RHEL LVM, SELinux enforcing boot smoke는 문서상 후속 검증 항목으로만 남긴다.

## 결과

좋음:

- 오픈 베타에서 데이터 손상 가능성이 높은 clone 경로를 차단한다.
- 커스텀 `storage_pool`에 있는 zvol도 실제 XML 경로 기준으로 clone할 수 있다.
- 기본 `zvol_pool`과 실제 원본 zvol 위치가 달라도 XML 기준으로 안전하게 clone plan을 계산한다.
- qcow2/raw와 libguestfs guest reset 구현 위치가 worker 경로로 연결됐다.
- Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol에서 clone, guest reset, boot smoke가 모두 통과했다.

주의:

- 이 결정은 clone 기능을 안전 조건 아래로 제한한다. 일반 VM clone은 필수 패키지 `libguestfs-tools` 설치와 source/disk 조건을 만족할 때만 열린다.
- `template_prepared=true`는 시스템이 sysprep을 수행한다는 뜻이 아니라, 운영자가 소스 VM을 템플릿 상태로 준비했다는 명시적 확인이다.
- 실환경 완료 범위는 ZFS zvol prepared template clone, Ubuntu 24.04 non-LVM qcow2/raw full clone + guest reset, Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone + guest reset이다.
- `guest_reset=true`는 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`가 설치된 Linux guest clone을 대상으로 한다. 비-Linux guest, 암호화된 guest disk, 여러 data disk는 이 ADR 범위가 아니다.
