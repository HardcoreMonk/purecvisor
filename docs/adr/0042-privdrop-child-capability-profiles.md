# ADR-0042: privdrop 이후 자식 capability 프로필

날짜: 2026-08-13
상태: Verified — 배포·운영 검증 완료
Single Edge 적용 상태: 활성
관계: ADR-0001, ADR-0026, ADR-0028

## 맥락

`pcv_privdrop_capabilities()`는 데몬의 Permitted/Effective/Inheritable 집합에서
`CAP_SETPCAP`을 먼저 제거한 뒤 `PR_CAPBSET_DROP`을 호출했다. 따라서 bounding-set
축소는 모두 `EPERM`이었고, UID 0인 데몬이 실행한 일반 자식은 Linux root exec 규칙으로
전체 bounding set을 다시 Effective/Permitted로 얻었다.

단순히 bounding set을 기존 데몬 keep-5로 줄이면 다음 제품 경로가 깨진다.

- DHCP `dnsmasq`: `CAP_NET_RAW`, `CAP_SETGID`, capability 유지 전환
- `kill`/`pkill`/`fuser`: 다른 UID 프로세스에 대한 `CAP_KILL`
- LXC와 libguestfs 계열: 컨테이너·appliance 런타임이 사용하는 넓은 capability 집합
- `tar`/ZFS 복원: 소유권·mode 복원에 필요한 파일 capability
- 커널 모듈: 데몬 자식 `modprobe`에 의존하면 `CAP_SYS_MODULE`이 필요

반대로 이 권한을 데몬 Effective 집합에 모두 남기면 최소권한의 의미가 사라진다.
또한 `fork()` 기반 별도 브로커는 ADR-0001의 단일 프로세스 원칙과 충돌한다.

## 결정

1. 데몬의 Effective 집합은 기존 keep-5를 유지한다. 자식 실행 준비를 위해 Permitted에는
   감사된 spawn ceiling을 두되, Effective에는 올리지 않는다.
2. bounding set은 LXC 기본 정책이 이미 제거하는 다섯 capability
   (`SYS_MODULE`, `SYS_RAWIO`, `SYS_TIME`, `MAC_OVERRIDE`, `MAC_ADMIN`)를 반드시 제거한다.
   `CAP_SETPCAP`이 Effective인 준비 단계에서 먼저 제거하고, 그 뒤 최종 데몬 집합을 적용한다.
3. 중앙 `pcv_spawn`은 `fork()` 후 `exec()` 전 child-setup에서 raw `capset`/`prctl`
   syscall만 사용해 자식별 profile을 적용한다. GLib·malloc·로그 등 fork 이후 비동기-signal
   안전성이 없는 호출은 사용하지 않는다.
4. 일반 자식은 `SECBIT_NOROOT|SECBIT_NOROOT_LOCKED`, exact bounding set, exact
   Permitted/Effective/Inheritable/Ambient를 적용한다. 따라서 UID 0 exec의 자동 전체
   capability 복원이 일어나지 않는다.
5. 프로필은 다음 다섯 종류다.

| 프로필 | 대상 | 추가 권한 |
|---|---|---|
| base | 일반 시스템 도구 | 기존 keep-5만 |
| storage | `zfs`, `zpool`, `tar`, 보존 복사 | `CHOWN`, `FOWNER`, `FSETID` |
| signal | `kill`, `pkill`, `fuser` | `KILL` |
| dhcp | `dnsmasq`, `ip netns exec … dnsmasq` | `CHOWN`, `SETGID`, `SETPCAP`, `NET_RAW` |
| runtime | `lxc-*`, libguestfs/QEMU appliance 경계 | 감사된 spawn ceiling |

6. runtime 프로필은 LXC가 컨테이너 capability를 자체 관리해야 하므로 `SECBIT_NOROOT`을
   잠그지 않는다. 그 외 프로필은 잠근다. runtime 프로필 대상은 basename allowlist로만
   선정하며 사용자 문자열로 프로필을 선택하지 않는다.
7. 중앙 helper를 우회하던 `GSubprocess` 직접 호출은 모두 `pcv_spawn`의 low-level 진입점으로
   수렴시킨다. hot reload의 자기 `execve()`는 자식 spawn이 아니라 프로세스 이미지 교체이므로
   예외다.
8. `CAP_SYS_MODULE`은 spawn ceiling에서 제외한다. 필요한 LIO와
   `nf_conntrack_bridge`는 `/etc/modules-load.d/purecvisor-lio.conf`가 부팅 전에 준비한다.
   데몬의 `modprobe`는 호환용 best-effort이며 실제 신규 적재 권한을 갖지 않는다.
9. capability 적용 실패는 자식에서 `_exit(126)`으로 fail-closed한다. 데몬 시작 시 bounding
   축소 실패는 성공으로 보고하지 않으며 journal에 보안 저하를 명시한다.
10. shell wrapper에 필요한 넓은 profile은 감사된 호출부가 compile-time enum 상수로만
    지정한다. custom environment는 고정 `PATH/HOME/LANG` 및 dynamic-loader/shell 시작
    hook을 덮을 수 없다. 정적 release gate가 두 계약과 중앙 spawn 우회를 차단한다.

## 대안

- **keep-5로 bounding 즉시 축소**: DHCP·LXC·정리 경로를 깨뜨려 기각.
- **모든 자식 필요 권한을 데몬 Effective에 유지**: 공격 표면이 넓어져 기각.
- **systemd transient unit으로 모든 명령 실행**: dnsmasq daemonize와 LXC cgroup/delegation
  계약이 달라지고, root D-Bus broker 자체의 allowlist가 별도 제품이 되어 현 범위에서는 기각.
- **별도 pre-fork privilege broker**: ADR-0001과 충돌하고 수명·IPC·감사 경계가 커져 기각.
- **libcap 호출을 child-setup에서 사용**: 멀티스레드 fork 이후 allocator/lock 안전성을
  보장하지 못해 raw syscall 방식을 채택.

## 결과

- 일반 외부 명령은 root exec 뒤에도 keep-5를 넘지 않는다.
- 실제로 추가 권한이 필요한 DHCP·signal·storage·runtime만 명시 프로필을 받는다.
- LXC 호환성을 위해 daemon Permitted/bounding ceiling은 keep-5보다 넓게 남는다. 이는
  `CAP_SYS_ADMIN`을 이미 보유하는 root orchestration daemon의 잔여 위험이며, 별도 비root
  broker가 생기기 전까지 수용한다.
- AppArmor 비부착과 NNP/seccomp 비활성이라는 ADR-0028 결정은 바꾸지 않는다.
