# Changelog — purecvisor-single

버전 문자열 단일 소스: `include/purecvisor/version.h` (`PCV_PRODUCT_VERSION`).
릴리스 태그: `vMAJOR.MINOR.PATCH`.

## v1.1.1 — 2026-07-06

패키징·빌드 위생 패치 릴리스 (신규 기능 없음). 런타임 버전 표시를 full semver(1.1.1)로 정합.

### Build / Packaging
- `make deb` 타깃 통합: `packaging/deb/build-deb.sh` 로 release 바이너리+UI+systemd 유닛+config sample 을 `dist/purecvisor-single_<ver>_amd64.deb` 로 조립. 버전은 version.h 파생, Depends 는 ldd→dpkg-query 자동 산출.
- UI 번들 결정화: `ui-bundle` 헤더의 `date` 타임스탬프 제거 → version.h 파생 버전+LOC. `make ui-bundle`/`make deb` 반복 실행 시 워킹 트리 diff 0.
- 빌드 의존 추적: Makefile `.d`(`-MMD`) 를 `-include` 하여 헤더(version.h 등) 변경이 증분 빌드에 반영.

### Fixes
- 기동 배너 버전을 리터럴("v1.0")에서 `PCV_PRODUCT_VERSION` 매크로로 (CLI/REST 와 일치).

### Docs
- GUIDE 2.2 `.deb` 바이너리 설치 절차(방법 A) 신설.

---

## v1.1.0 — 2026-07-06

1.0 안정화 사이클 결과를 minor 릴리스로 패키징. 신규 네트워크 기능 + dogfooding 수정 + 하드닝/operate 잔여 소진.
상세 근거: `docs/operations/2026-07-06-session-split-1.0-stabilization-handoff.md`, `docs/operations/2026-07-05-vp-series-release-handoff.md`, `docs/operations/2026-07-05-vm-provisioning-dogfooding-findings.md`.

### Features
- **관리형 기본 NAT 네트워크 `pcvnat0`** (VP-1): 데몬 기동 시 브릿지+NAT(nftables)+DHCP/DNS(dnsmasq)를 멱등 보장. `vm create` 에서 `--network_bridge` 미지정 시 자동 부착, `none` 으로 opt-out. 신규 `[network]` config 섹션(`default_bridge`/`default_subnet`/`default_ensure`/`firewall_integration`).
- **호스트 방화벽 자동 공존** (VP-6): UFW/iptables-DROP 감지 후 게스트 포워딩·DHCP·DNS 경로를 자동 개통(AUDIT 추적). `[network] firewall_integration = auto|off`. firewalld 는 감지·경고.
- **guest-agent 채널 기본 포함** (VP-2): `vm create` 도메인 XML 에 virtio-serial 채널을 무조건 추가 → `guest-ping`/`guest-exec`/`guest-shutdown` 이 self-created VM 에서 동작.
- **SG scoped-nft 재설계**: bridge `pcv_sg` 스코프 체인 + vnet 캐시(라이프사이클 evict/주기 resync/NIC 핫플러그 훅) + DB fail-closed 가드. followups(I2-R1~R3, R4~R12, M-2~M-10) 전량 소진.

### Fixes
- guest-exec exitcode 신뢰성 (VP-3): CLI 조회 키 정정 + 데몬 `guest-exec-status` 폴링 루프(exited 판정).
- CLI 견고성: `vm delete-status` 값-노드 파싱(VP-4), `network list` 키 `name`/`ip_cidr`(VP-5), `security-group rule add` direction `in`/`out` 별칭 정규화(VP-7), `vm create --help/-h` 가드 + 플래그형 이름 거부(VP-8).
- dnsmasq 데몬 재시작 관통 생존 (VP-6/B-3): KillMode=process + 생존 게이트 `kill(0)`→`/proc/<pid>/comm`(privdrop CAP_KILL 상실 대응). DHCP 공백 0.
- 기본 네트워크 DHCP 를 DNS 포워더 포함으로 기동 — 게스트 이름 해석 가능.

### Hardening
- launcher shutdown TOCTOU 를 spawn 뮤텍스로 동기화 (B-1).
- `evidence_json` 오버플로 가드를 프로듀서+역직렬화 site 공용 함수로 통일 (B-2).

### Ops / Docs
- `daemon.conf` `[network]`/`[security_group]` 레퍼런스 + 구 `[vm] default_bridge` 폐기 마이그레이션 노트 (B-4).
- 무인증 `/api/v1/health` liveness 모니터링 연결 + `rest_port` 충돌(REST 다운) 복구 (B-6).
- 테스트 config 격리 `PCV_CONFIG_PATH` (B-5).

### Known / Deferred
- VP-9 (클라우드 이미지 lsilogic SCSI 컨트롤러): 표준 pcvctl 경로 무영향 — 2.0 클라우드 이미지 지원 시 virtio-scsi 기본 검토.

### Migration
- `daemon.conf` 의 구 `[vm] default_bridge` 는 폐기 → `[network] default_bridge`(기본 `pcvnat0`)로 이관. 구 키는 조용히 무시됨.
- 신규 배포에서 호스트 방화벽 자동 개통이 싫으면 `[network] firewall_integration = off`.
