# systemd 배포 요구사항

> **2026-10-02 소스 기준:** 공개 제품 로직을 개발 main에 맞췄다. NVRAM·첫 알림 쿨다운 시정은 유지하며 LXC는 ZFS 전용이다. 공개 전용 Btrfs와 개발 main 미병합 후속 브랜치는 포함하지 않는다. [정합화 인계](../../docs/operations/2026-10-02-public-main-sync-handoff.md)에서 이번 검증을 구분한다.


이 디렉터리는 PureCVisor Single Edge 2.1.0의 보조 systemd 유닛과
`purecvisorsd.service` 드롭인을 보관한다. 전체 절차는
[운영 가이드](../../docs/GUIDE.md)와
[검증 정책](../../docs/DEVELOPMENT_VERIFICATION_POLICY.md)을 기준으로 한다.

## 파일

| 파일 | 역할 |
|------|------|
| `90-ovl3-netns.conf` | 데몬이 호스트 mount namespace를 사용하도록 충돌하는 systemd 격리를 해제 |
| `95-coredump-hardening.conf` | 기본 core dump 제한 |
| `96-hidepid.conf` | 데몬에 `pcvmon` 보조 그룹 부여 |
| `97-sandbox.conf` | mount namespace를 만들지 않는 서비스 샌드박스 옵션 |
| `proc-hidepid.service` | `/proc`을 `hidepid=2,gid=pcvmon`으로 재마운트 |
| `purecvisor-host-tuning.service` | Single Edge 호스트 런타임 튜닝 |
| `purecvisor-lxc@.service` | 컨테이너 하나를 PID 1의 자식으로 띄우는 템플릿 유닛 |
| `suricata.service` | 별도 Suricata IDS 인스턴스 |
| `suricata-ips.service` | 선택형 NFQUEUE IPS 인스턴스 |

## 필수 운영 조건

1. `purecvisorsd`는 네트워크 namespace 수명주기를 관리하므로 호스트 mount
   namespace를 사용해야 한다. 데몬 유닛에 `ProtectSystem`, `ProtectHome`,
   `PrivateTmp`, `PrivateDevices`, `ProtectKernelTunables`,
   `ProtectControlGroups`, `ReadOnlyPaths`, `ReadWritePaths`,
   `InaccessiblePaths`, `TemporaryFileSystem`, `PrivateMounts`를 추가하지 않는다.
   서비스 샌드박스 드롭인 `97-sandbox.conf`는 mount namespace를 만들지 않는 옵션만 둔다 —
   `NotifyAccess=main`(sd_notify는 데몬 본 프로세스만 보낸다), `SystemCallArchitectures=native`,
   `LockPersonality=yes`, `RestrictRealtime=yes`,
   `SystemCallFilter=~@swap @obsolete @cpu-emulation @module @raw-io`. `ProtectKernelModules`·
   `ProtectKernelLogs`도 mount namespace를 만들므로 쓰지 않고, 모듈 적재 차단은 `@module` 필터로
   대신한다. 컨테이너 init은 전용 유닛 `purecvisor-lxc@.service`가 띄우므로 이 필터를 상속하지
   않지만 `lxc-attach`로 실행하는 명령은 상속한다 — 그 명령으로 실행한 32비트 i386 바이너리는
   SIGSYS로, `setarch i686`은 EPERM으로 실패한다. 드롭인을 지우고 `systemctl daemon-reload`와
   `systemctl restart purecvisorsd`를 하면 원복된다.
2. WireGuard overlay에는 `wireguard-tools`, `iproute2`, `dnsmasq`, 커널
   WireGuard 모듈이 필요하다. TLS 자동 프로비저닝에는 `openssl` CLI가 필요하다.
3. 외부 API는 HTTPS 443/tcp를 사용한다. 데몬의 평문 HTTP 기본값은
   `127.0.0.1:8080`이며 외부 방화벽에 열지 않는다. 선택형 NGINX의 HTTP 80
   리다이렉트와 데몬의 로컬 HTTP 리스너는 별개다.
4. BPF 빌드에는 `clang`, `bpftool`, `libbpf-dev`와 커널 BTF가 필요하다.
   런타임에는 `pcv_lsm.bpf.o`, `pcv_shared_bridge.bpf.o`, `manifest.json`을
   함께 배포한다.
5. 고급 QoS에는 `sch_hfsc`, `sch_cake`, `sch_netem`, `ifb`,
   `act_mirred`, `cls_flower` 커널 기능이 필요하다.
6. iSCSI target에는 `packaging/deb/purecvisor-lio.conf`의 LIO 모듈이 필요하다.
7. Suricata IDS와 IPS는 데몬과 별도 유닛으로 실행한다. 미설치는 해당 기능의
   degraded 상태이며 데몬 자체의 기동을 막지 않는다.

## 드롭인과 hidepid 설치

```bash
sudo groupadd --system pcvmon 2>/dev/null || true
sudo install -d /etc/systemd/system/purecvisorsd.service.d
sudo install -m 0644 packaging/systemd/90-ovl3-netns.conf \
  /etc/systemd/system/purecvisorsd.service.d/90-ovl3-netns.conf
sudo install -m 0644 packaging/systemd/95-coredump-hardening.conf \
  /etc/systemd/system/purecvisorsd.service.d/95-coredump-hardening.conf
sudo install -m 0644 packaging/systemd/96-hidepid.conf \
  /etc/systemd/system/purecvisorsd.service.d/96-hidepid.conf
sudo install -m 0644 packaging/systemd/97-sandbox.conf \
  /etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf
sudo install -m 0644 packaging/systemd/proc-hidepid.service \
  /etc/systemd/system/proc-hidepid.service
sudo systemctl daemon-reload
sudo systemctl restart purecvisorsd
sudo systemctl enable --now proc-hidepid.service
```

롤백은 `sudo systemctl stop proc-hidepid.service`로 수행한다. 이 유닛의
`ExecStop`은 `hidepid=0` 재마운트를 명시한다. 드롭인 세 개(`95`·`96`·`97`)와
`proc-hidepid.service`는 `scripts/install-daemon-dropins.sh`로도 같은 경로에 설치한다.

`96-hidepid.conf`와 `proc-hidepid.service`는 짝으로 설치·제거한다. 실행 중 컨테이너 볼륨
detach의 최종 확인은 컨테이너 cgroup의 모든 pid·스레드의 `/proc/<pid>/task/<tid>/mountinfo`를
읽고 사라진 pid는 건너뛴다. `proc-hidepid.service`만 켜고 드롭인(`SupplementaryGroups=pcvmon`)이
없으면 데몬이 일부 컨테이너 프로세스를 `/proc`에서 보지 못해 그 프로세스가 쥔 볼륨 사본을 놓칠 수
있다. hidepid를 걸 때 데몬 `Groups`에 pcvmon이 있는지 아래 검증으로 함께 확인한다.

## 컨테이너 전용 유닛

`purecvisor-lxc@.service`는 컨테이너 하나를 PID 1의 자식으로 띄우는 템플릿 유닛이다. deb가
`/etc/systemd/system/`에 conffile로 설치하고 `scripts/deploy.sh`도 같은 경로에 설치한다.
`[Install]`이 없어 데몬의 `container.start`만 시작한다.

- `ExecStart`의 `-f /run/purecvisor/lxc-runtime/%i/config`는 데몬이 시작마다 쓰는 런타임 config다.
  영속 config(`${PCV_LXC_PATH}/%i/config`)와 같고, 볼륨 줄의 원본만 데몬이 링크 없이 확인해 붙인
  root 전용 staging(`/run/purecvisor-volstage/%i/`)을 가리킨다. `scripts/check_deb_supply_chain.py`가
  이 `ExecStart` 키 목록을 `-f`까지 고정한다.
- 유닛·데몬·`pcvctl`은 함께 반영하고 함께 되돌린다. 옛 유닛(`-f` 없음)이 남으면 볼륨 컨테이너는
  시작 검사 훅이 거절하고, 새 유닛에 옛 데몬이면 런타임 config가 없어 모든 컨테이너 시작이 실패한다.
  되돌리기 전에 볼륨 컨테이너를 정지한다.
- 데몬 밖 시작은 지원 경로가 아니다. 런타임 config가 없으면 `systemctl start purecvisor-lxc@<이름>`은
  실패한다. 컨테이너 안 poweroff처럼 데몬이 정지를 처리하지 않은 뒤에는 이전 시작의 런타임 config와
  staging이 `/run`에 남아 직접 시작하면 그 이전 staging으로 켜지므로, 시작은 `container.start`로 한다.
- 시작이 실패하면 `journalctl -u purecvisor-lxc@<이름>`과 컨테이너 디렉터리의 `<이름>.log`를 본다.

## 호스트 튜닝 유닛

`purecvisor-host-tuning.service`는 best-effort oneshot이다. 시작할 때 CPU governor를 `performance`로,
THP defrag를 `defer+madvise`로 쓰고, stop할 때 `powersave`/`madvise`로 되돌린다. sysfs 경로가 없거나
쓰기가 실패해도 부팅을 막지 않으므로 유닛 성공만으로 적용됐다고 보지 말고 각 노드의 현재 값을 읽어
확인한다. 컨테이너 안에서는 실행하지 않는다(`ConditionVirtualization=!container`). deb postinst와
`scripts/deploy.sh`가 배포마다 enable을 다시 적용하므로 `systemctl disable purecvisor-host-tuning`
옵트아웃은 다음 배포 전까지만 유효하다.

## Suricata 설치

```bash
sudo install -m 0644 packaging/systemd/suricata.service \
  /etc/systemd/system/suricata.service
sudo install -m 0644 packaging/systemd/suricata-ips.service \
  /etc/systemd/system/suricata-ips.service
sudo systemctl daemon-reload
sudo systemctl enable --now suricata
```

`suricata-ips.service`의 `-q 0`과 `daemon.conf`의 `[ips] queue_num`은
같아야 한다. IPS 유닛은 제품 설정과 RPC가 시작·중지를 관리하므로 별도로
enable하지 않는다.

## 검증

```bash
systemctl is-active purecvisorsd
systemctl show purecvisorsd -p LimitCORESoft -p LimitCOREHard
grep ' /proc ' /proc/mounts | grep hidepid
grep ^Groups /proc/$(pidof purecvisorsd)/status
readlink /proc/1/ns/mnt
readlink /proc/$(pidof purecvisorsd)/ns/mnt
curl -sk https://127.0.0.1/health | jq .
```

두 mount namespace 값은 같아야 한다. 배포 후 기능 검증은
[서비스 기능 테스트 시나리오](../../docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md)를
따른다.
