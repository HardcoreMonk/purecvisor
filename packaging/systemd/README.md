# systemd 배포 요구사항

이 디렉터리는 PureCVisor Single Edge 2.0.0의 보조 systemd 유닛과
`purecvisorsd.service` 드롭인을 보관한다. 전체 절차는
[운영 가이드](../../docs/GUIDE.md)와
[검증 정책](../../docs/DEVELOPMENT_VERIFICATION_POLICY.md)을 기준으로 한다.

## 파일

| 파일 | 역할 |
|------|------|
| `90-ovl3-netns.conf` | 데몬이 호스트 mount namespace를 사용하도록 충돌하는 systemd 격리를 해제 |
| `95-coredump-hardening.conf` | 기본 core dump 제한 |
| `96-hidepid.conf` | 데몬에 `pcvmon` 보조 그룹 부여 |
| `proc-hidepid.service` | `/proc`을 `hidepid=2,gid=pcvmon`으로 재마운트 |
| `purecvisor-host-tuning.service` | Single Edge 호스트 런타임 튜닝 |
| `suricata.service` | 별도 Suricata IDS 인스턴스 |
| `suricata-ips.service` | 선택형 NFQUEUE IPS 인스턴스 |

## 필수 운영 조건

1. `purecvisorsd`는 네트워크 namespace 수명주기를 관리하므로 호스트 mount
   namespace를 사용해야 한다. 데몬 유닛에 `ProtectSystem`, `ProtectHome`,
   `PrivateTmp`, `PrivateDevices`, `ProtectKernelTunables`,
   `ProtectControlGroups`, `ReadOnlyPaths`, `ReadWritePaths`,
   `InaccessiblePaths`, `TemporaryFileSystem`, `PrivateMounts`를 추가하지 않는다.
2. WireGuard overlay에는 `wireguard-tools`, `iproute2`, `dnsmasq`, 커널
   WireGuard 모듈이 필요하다. TLS 자동 프로비저닝에는 `openssl` CLI가 필요하다.
3. 외부 API는 HTTPS 443/tcp를 사용한다. HTTP 80/tcp는 기본적으로 loopback
   전용이며 외부 방화벽에 열지 않는다.
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
sudo install -m 0644 packaging/systemd/proc-hidepid.service \
  /etc/systemd/system/proc-hidepid.service
sudo systemctl daemon-reload
sudo systemctl restart purecvisorsd
sudo systemctl enable --now proc-hidepid.service
```

롤백은 `sudo systemctl stop proc-hidepid.service`로 수행한다. 이 유닛의
`ExecStop`은 `hidepid=0` 재마운트를 명시한다.

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
